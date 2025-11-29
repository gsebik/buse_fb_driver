/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Buse 128x19 SPI framebuffer driver
 *
 * This driver exposes a 1bpp Linux framebuffer (/dev/fbX) and
 * periodically converts that linear framebuffer into the custom
 * SPI data format required by a 4-panel, 128x19 LED display.
 *
 * The display is driven in 4 "groups" of columns. For each group:
 *   - We send a chunk of bytes for all 4 panels over SPI.
 *   - Then pull CS low for a fixed time (DISPLAY_BRIGHTNESS_USEC),
 *     which effectively controls LED brightness.
 *   - After the delay, we reassert CS and send the next group.
 *
 * The conversion pipeline is:
 *   info->screen_base (linear 1bpp)  --->  shadow_vram (snapshot)
 *   ---> frame_buffer (panel/group-specific layout) ---> SPI
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fb.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/math64.h>

/* Duration (in microseconds) that CS stays low after each transfer.
 * This effectively controls display brightness.
 */
#define DISPLAY_BRIGHTNESS_USEC 50

/*
 * Configuration computed at probe time from DT + defaults.
 * All the geometry and derived sizes live here.
 */
struct busefb_config {
	u32 width;         /* logical width in pixels (e.g. 128) */
	u32 height;        /* logical height in pixels (e.g. 19) */
	u32 panels;        /* number of horizontal panels (e.g. 4) */

	u32 regs_per_col;  /* bytes per column = ceil(height / 8) */
	u32 panel_cols;    /* columns per panel = width / panels */

	u32 cols_per_group;/* columns per group inside a panel = panel_cols / 4
	                      (because we drive 4 groups, each group handles 1/4
	                       of the columns for that panel) */

	u32 panel_bytes;   /* bytes for one (panel, group) block:
	                      = 1 (header) + cols_per_group * regs_per_col */
	u32 group_bytes;   /* bytes for one group covering all panels:
	                      = panels * panel_bytes */
	u32 frame_bytes;   /* bytes for full frame:
	                      = 4 groups * group_bytes (always 4 groups) */
};

/*
 * Per-device private data.
 */
struct busefb_par {
	struct spi_device *spi;          /* SPI device handle */
	struct fb_info *info;            /* framebuffer info */
	struct gpio_desc *cs_gpio;       /* manual CS GPIO (gpiod API) */

	struct workqueue_struct *wq;     /* single-threaded workqueue */
	struct work_struct refresh_work; /* builds frame and kickoff SPI */
	struct work_struct cs_reassert_work; /* reassert CS and send next group */
	struct hrtimer cs_delay_timer;   /* hrtimer for CS-low delay */
	spinlock_t fb_lock;              /* protects screen_base while copying */

	struct busefb_config config;     /* geometry/size info */

	/* Buffers:
	 *  - frame_buffer: hardware-specific packed format sent over SPI
	 *  - shadow_vram: snapshot of linear framebuffer before conversion
	 */
	u8 *frame_buffer;
	u8 *shadow_vram;

	/* Which group (0..3) is currently being sent. */
	int current_group;
};

/* Forward declaration. */
static void process_next_group(struct busefb_par *par);

/*
 * Work item: reassert CS after the brightness delay and either
 * send the next group or schedule a new refresh.
 */
static void cs_reassert_work_func(struct work_struct *work)
{
	struct busefb_par *par =
		container_of(work, struct busefb_par, cs_reassert_work);

	/* End brightness pulse: CS high again. */
	gpiod_set_value(par->cs_gpio, 1);

	/* Move to next group of data. */
	par->current_group++;

	if (par->current_group >= 4) { /* There are always 4 groups */
		/* We sent all groups: reset for next frame. */
		par->current_group = 0;
		/* Build and send a new frame. */
		queue_work(par->wq, &par->refresh_work);
	} else {
		/* Continue with the next group immediately. */
		process_next_group(par);
	}
}

/*
 * Hrtimer callback: runs when the CS-low delay expires.
 * We cannot do heavy work here, so we just queue a work item.
 */
static enum hrtimer_restart cs_delay_timer_callback(struct hrtimer *timer)
{
	struct busefb_par *par =
		container_of(timer, struct busefb_par, cs_delay_timer);

	/* Defer the real work (CS reassert and next group) to workqueue. */
	queue_work(par->wq, &par->cs_reassert_work);
	return HRTIMER_NORESTART;
}

/*
 * Send the SPI data for the current group (par->current_group).
 * This:
 *   - Raises CS
 *   - Performs synchronous SPI transfer for one group
 *   - Pulls CS low
 *   - Starts hrtimer for the brightness delay
 */
static void process_next_group(struct busefb_par *par)
{
	struct busefb_config *cfg = &par->config;
	int g = par->current_group;
	struct spi_transfer t = {
		/* Pointer to this group's data within frame_buffer */
		.tx_buf = par->frame_buffer + g * cfg->group_bytes,
		.len = cfg->group_bytes,
		.speed_hz = par->spi->max_speed_hz,
	};
	struct spi_message m;

	/* CS high during transfer. */
	gpiod_set_value(par->cs_gpio, 1);

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	/* Blocking transfer for one group. */
	spi_sync(par->spi, &m);

	/* CS low after transfer: start brightness pulse window. */
	gpiod_set_value(par->cs_gpio, 0);

	/* Start a relative hrtimer for DISPLAY_BRIGHTNESS_USEC microseconds. */
	hrtimer_start(&par->cs_delay_timer,
		      ktime_set(0, DISPLAY_BRIGHTNESS_USEC * 1000),
		      HRTIMER_MODE_REL_PINNED);
}

/*
 * Work item: build a complete frame from the Linux framebuffer and
 * start sending it (begin with group 0).
 */
static void refresh_work_func(struct work_struct *work)
{
	struct busefb_par *par =
		container_of(work, struct busefb_par, refresh_work);
	struct busefb_config *cfg = &par->config;
	unsigned long flags;

	/*
	 * Snapshot the current contents of the framebuffer into shadow_vram,
	 * and clear the frame_buffer before we fill it.
	 *
	 * Protected by fb_lock in case the framebuffer is modified from
	 * other contexts while we are copying.
	 */
	spin_lock_irqsave(&par->fb_lock, flags);
	memcpy(par->shadow_vram,
	       par->info->screen_base,
	       par->info->fix.smem_len);
	memset(par->frame_buffer, 0, cfg->frame_bytes);
	spin_unlock_irqrestore(&par->fb_lock, flags);

	/*
	 * Convert from logical (x,y) pixel coordinates into the display's
	 * weird column/panel/group/register layout in frame_buffer.
	 *
	 * Logical framebuffer layout (1bpp bit-packed):
	 *   shadow_vram[byte] bit[0..7]
	 *   idx = y * width + x (before mirroring)
	 */
	for (int y = 0; y < cfg->height; y++) {
		for (int x = 0; x < cfg->width; x++) {
			/* Mirror horizontally to match physical orientation. */
			int x_mirrored = cfg->width - 1 - x;
			int idx = y * cfg->width + x_mirrored;

			/* Check if this pixel is set in shadow_vram. */
			if (!(par->shadow_vram[idx >> 3] & (1 << (idx & 7))))
				continue; /* Pixel off: skip. */

			/* Vertical flip: hardware expects reversed order. */
			int y_rev = cfg->height - 1 - y;

			/* Which "register" (byte) in the column this pixel is in. */
			int reg = y_rev / 8;

			/* Bit position within that byte (top pixel → MSB). */
			int bit = 7 - (y_rev % 8);

			/* Determine which panel this x belongs to. */
			int panel = x / cfg->panel_cols;

			/* Group index (0..3) based on x modulo 4. */
			int grp = x % 4;

			/*
			 * Column index within the current panel.
			 * We divide by 4 because each group covers 1/4 of the
			 * panel columns. Then XOR with 1 to match the display's
			 * column ordering (swapped pairs).
			 */
			int cp = (x % cfg->panel_cols) / 4 ^ 0x01;

			/*
			 * Base index into frame_buffer for this (group, panel).
			 *
			 * Layout is:
			 *   [group 0: panel 0][group 0: panel 1]...[group 0: panel N-1]
			 *   [group 1: panel 0]...
			 *
			 * Each (group, panel) block has panel_bytes bytes:
			 *   byte 0: group header (grp)
			 *   byte 1..: column register data
			 */
			int base = grp * cfg->group_bytes + panel * cfg->panel_bytes;

			/* First byte in each panel block encodes the group ID. */
			par->frame_buffer[base] = grp;

			/*
			 * Compute index within the data region (after header):
			 *
			 *   1 + cp * regs_per_col + reg
			 *
			 * cp: which column bucket within this panel/group
			 * reg: which vertical byte (of regs_per_col) in that column
			 */
			par->frame_buffer[base + 1 + cp * cfg->regs_per_col + reg] |=
				1 << bit;
		}
	}

	/* Start sending from group 0. */
	par->current_group = 0;
	process_next_group(par);
}

/* Standard framebuffer operations for sysmem-based 1bpp buffer. */
static const struct fb_ops busefb_ops = {
	.owner       = THIS_MODULE,
	.fb_read     = fb_sys_read,
	.fb_write    = fb_sys_write,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

/*
 * SPI probe: allocate resources, compute layout, register framebuffer,
 * and kick off the first refresh.
 */
static int busefb_probe(struct spi_device *spi)
{
	struct fb_info *info;
	struct busefb_par *par;
	struct busefb_config *cfg;
	int ret;
	u32 width = 128, height = 19, panels = 4;

	/* Allocate framebuffer info plus private data (par). */
	info = framebuffer_alloc(sizeof(struct busefb_par), &spi->dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	par->spi = spi;
	par->info = info;
	cfg = &par->config;
	spin_lock_init(&par->fb_lock);

	/* Read optional geometry overrides from device tree / firmware. */
	device_property_read_u32(&spi->dev, "width", &width);
	device_property_read_u32(&spi->dev, "height", &height);
	device_property_read_u32(&spi->dev, "panels", &panels);

	/* Fill in configuration struct. */
	cfg->width = width;
	cfg->height = height;
	cfg->panels = panels;

	/* Number of vertical bytes per column (ceil(height / 8)). */
	cfg->regs_per_col = DIV_ROUND_UP(height, 8);

	/* Columns per panel and per group. */
	cfg->panel_cols = width / panels;
	cfg->cols_per_group = cfg->panel_cols / 4; /* 4 groups */

	/*
	 * Layout for one (panel, group) block:
	 *   1 header byte + (cols_per_group * regs_per_col) data bytes.
	 */
	cfg->panel_bytes = (cfg->cols_per_group * cfg->regs_per_col) + 1;

	/* Bytes for one group (all panels). */
	cfg->group_bytes = panels * cfg->panel_bytes;

	/* 4 groups form a full frame. */
	cfg->frame_bytes = 4 * cfg->group_bytes;

	/* Allocate frame_buffer used for SPI transfers. */
	par->frame_buffer = vzalloc(cfg->frame_bytes);
	if (!par->frame_buffer) {
		ret = -ENOMEM;
		goto err_release;
	}

	/* Request CS GPIO (output, initially high). */
	par->cs_gpio = devm_gpiod_get(&spi->dev, "cs", GPIOD_OUT_HIGH);
	if (IS_ERR(par->cs_gpio)) {
		ret = PTR_ERR(par->cs_gpio);
		goto err_free_frame_buffer;
	}

	/* Describe the fixed (hardware) aspects of the framebuffer. */
	info->fix = (struct fb_fix_screeninfo){
		.id = "busefb",
		.type = FB_TYPE_PACKED_PIXELS,
		.visual = FB_VISUAL_MONO01,
		.line_length = width / 8,       /* bytes per line at 1bpp */
		.smem_len = width * height / 8, /* total bytes for framebuffer */
	};

	/* Describe variable (mode) aspects. */
	info->var = (struct fb_var_screeninfo){
		.bits_per_pixel = 1,
		.xres = width,
		.yres = height,
		.xres_virtual = width,
		.yres_virtual = height,
	};

	info->fbops = &busefb_ops;

	/* Allocate the linear framebuffer memory exposed to userspace. */
	info->screen_base = vzalloc(info->fix.smem_len);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto err_free_frame_buffer;
	}

	/* Allocate shadow_vram used during conversion. Same size as screen_base. */
	par->shadow_vram = vzalloc(info->fix.smem_len);
	if (!par->shadow_vram) {
		ret = -ENOMEM;
		goto err_free_screen_base;
	}

	/* Create a single-threaded workqueue for refresh and CS logic. */
	par->wq = create_singlethread_workqueue("busefb_wq");
	if (!par->wq) {
		ret = -ENOMEM;
		goto err_free_shadow_vram;
	}

	INIT_WORK(&par->refresh_work, refresh_work_func);
	INIT_WORK(&par->cs_reassert_work, cs_reassert_work_func);

	/* Initialize hrtimer used for CS-low delay. */
	hrtimer_init(&par->cs_delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	par->cs_delay_timer.function = cs_delay_timer_callback;

	/* Register framebuffer with the kernel. */
	ret = register_framebuffer(info);
	if (ret)
		goto err_destroy_wq;

	/* Attach driver data to SPI device for later retrieval. */
	spi_set_drvdata(spi, par);

	/* Start the first frame build+send. */
	queue_work(par->wq, &par->refresh_work);

	dev_info(&spi->dev, "busefb registered as /dev/fb%d\n", info->node);
	return 0;

err_destroy_wq:
	destroy_workqueue(par->wq);
err_free_shadow_vram:
	vfree(par->shadow_vram);
err_free_screen_base:
	vfree(info->screen_base);
err_free_frame_buffer:
	vfree(par->frame_buffer);
err_release:
	framebuffer_release(info);
	return ret;
}

/*
 * Remove callback: stop timers, unregister fb, and free resources.
 */
static void busefb_remove(struct spi_device *spi)
{
	struct busefb_par *par = spi_get_drvdata(spi);

	hrtimer_cancel(&par->cs_delay_timer);
	unregister_framebuffer(par->info);
	destroy_workqueue(par->wq);
	vfree(par->shadow_vram);
	vfree(par->info->screen_base);
	vfree(par->frame_buffer);
	framebuffer_release(par->info);
}

/* Device tree match table. */
static const struct of_device_id busefb_of_match[] = {
	{ .compatible = "buse,buse128x19" },
	{ }
};
MODULE_DEVICE_TABLE(of, busefb_of_match);

/* SPI driver registration. */
static struct spi_driver busefb_driver = {
	.driver = {
		.name = "busefb",
		.of_match_table = busefb_of_match,
	},
	.probe = busefb_probe,
	.remove = busefb_remove,
};
module_spi_driver(busefb_driver);

MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Buse 128x19 SPI framebuffer driver");
MODULE_LICENSE("GPL");
