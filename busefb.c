/* SPDX-License-Identifier: GPL-2.0 */
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

#define WIDTH 128
#define HEIGHT 19
#define PANELS 4
#define PANEL_COLS (WIDTH / PANELS)
#define GROUPS 4
#define COLS_PER_GROUP (PANEL_COLS / GROUPS)
#define REGS_PER_COL 3
#define DATA_BYTES_PER_PANEL (COLS_PER_GROUP * REGS_PER_COL)
#define PANEL_BYTES (DATA_BYTES_PER_PANEL + 1)
#define GROUP_BYTES (PANELS * PANEL_BYTES)
#define FRAME_BYTES (GROUPS * GROUP_BYTES)

#define DISPLAY_BRIGHTNESS_USEC 50

struct busefb_par {
	struct spi_device *spi;
	struct fb_info *info;
	struct gpio_desc *cs_gpio;
	struct workqueue_struct *wq;
	struct work_struct refresh_work;
	struct work_struct cs_reassert_work;
	struct hrtimer cs_delay_timer;
	spinlock_t fb_lock;

	u8 frame_buffer[FRAME_BYTES];
	u8 *shadow_vram;
	int current_group;
};

static void process_next_group(struct busefb_par *par);

static void cs_reassert_work_func(struct work_struct *work)
{
	struct busefb_par *par = container_of(work, struct busefb_par, cs_reassert_work);
	gpiod_set_value(par->cs_gpio, 1);
	par->current_group++;

	if (par->current_group >= GROUPS) {
		par->current_group = 0;
		queue_work(par->wq, &par->refresh_work);
	} else {
		process_next_group(par);
	}
}

static enum hrtimer_restart cs_delay_timer_callback(struct hrtimer *timer)
{
	struct busefb_par *par = container_of(timer, struct busefb_par, cs_delay_timer);
	queue_work(par->wq, &par->cs_reassert_work);
	return HRTIMER_NORESTART;
}

static void process_next_group(struct busefb_par *par)
{
	int g = par->current_group;
	struct spi_transfer t = {
		.tx_buf = par->frame_buffer + g * GROUP_BYTES,
		.len = GROUP_BYTES,
		.speed_hz = par->spi->max_speed_hz,
	};
	struct spi_message m;

	gpiod_set_value(par->cs_gpio, 1);
	
	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	spi_sync(par->spi, &m);

	gpiod_set_value(par->cs_gpio, 0);
	hrtimer_start(&par->cs_delay_timer, ktime_set(0, DISPLAY_BRIGHTNESS_USEC * 1000), HRTIMER_MODE_REL_PINNED);
}

static void refresh_work_func(struct work_struct *work)
{
	struct busefb_par *par = container_of(work, struct busefb_par, refresh_work);
	unsigned long flags;

	spin_lock_irqsave(&par->fb_lock, flags);
	memcpy(par->shadow_vram, par->info->screen_base, par->info->fix.smem_len);
	memset(par->frame_buffer, 0, FRAME_BYTES);
	spin_unlock_irqrestore(&par->fb_lock, flags);

	for (int y = 0; y < HEIGHT; y++) {
		for (int x = 0; x < WIDTH; x++) {
			int x_mirrored = WIDTH - 1 - x;
			int idx = y * WIDTH + x_mirrored;
			if (!(par->shadow_vram[idx >> 3] & (1 << (idx & 7))))
				continue;

			int y_rev = HEIGHT - 1 - y;
			int reg = y_rev / 8;
			int bit = 7 - (y_rev % 8);
			int panel = x / PANEL_COLS;
			int grp = x % GROUPS;
			int cp = (x % PANEL_COLS) / GROUPS ^ 0x01;
			int base = grp * GROUP_BYTES + panel * PANEL_BYTES;
			par->frame_buffer[base] = grp;
			par->frame_buffer[base + 1 + cp * REGS_PER_COL + reg] |= 1 << bit;
		}
	}

	par->current_group = 0;
	process_next_group(par);
}

static const struct fb_fix_screeninfo busefb_fix = {
	.id = "busefb",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_MONO01,
	.line_length = WIDTH / 8,
	.smem_len = WIDTH * HEIGHT / 8,
};

static const struct fb_var_screeninfo busefb_var = {
	.bits_per_pixel = 1,
	.xres = WIDTH,
	.yres = HEIGHT,
	.xres_virtual = WIDTH,
	.yres_virtual = HEIGHT,
};

static const struct fb_ops busefb_ops = {
	.owner = THIS_MODULE,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
};

static int busefb_probe(struct spi_device *spi)
{
	struct fb_info *info;
	struct busefb_par *par;
	int ret;

	info = framebuffer_alloc(sizeof(struct busefb_par), &spi->dev);
	if (!info)
		return -ENOMEM;

	par = info->par;
	par->spi = spi;
	par->info = info;
	spin_lock_init(&par->fb_lock);

	par->cs_gpio = devm_gpiod_get(&spi->dev, "cs", GPIOD_OUT_HIGH);
	if (IS_ERR(par->cs_gpio)) {
		ret = PTR_ERR(par->cs_gpio);
		goto err_release;
	}

	info->fix = busefb_fix;
	info->var = busefb_var;
	info->fbops = &busefb_ops;
	info->screen_base = vzalloc(info->fix.smem_len);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto err_release;
	}

	par->shadow_vram = vzalloc(info->fix.smem_len);
	if (!par->shadow_vram) {
		ret = -ENOMEM;
		goto err_vfree_screen_base;
	}

	par->wq = create_singlethread_workqueue("busefb_wq");
	if (!par->wq) {
		ret = -ENOMEM;
		goto err_vfree_shadow_vram;
	}

	INIT_WORK(&par->refresh_work, refresh_work_func);
	INIT_WORK(&par->cs_reassert_work, cs_reassert_work_func);
	hrtimer_init(&par->cs_delay_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	par->cs_delay_timer.function = cs_delay_timer_callback;

	ret = register_framebuffer(info);
	if (ret)
		goto err_destroy_wq;

	spi_set_drvdata(spi, par);
	queue_work(par->wq, &par->refresh_work);

	dev_info(&spi->dev, "busefb registered as /dev/fb%d\n", info->node);
	return 0;

err_destroy_wq:
	destroy_workqueue(par->wq);
err_vfree_shadow_vram:
	vfree(par->shadow_vram);
err_vfree_screen_base:
	vfree(info->screen_base);
err_release:
	framebuffer_release(info);
	return ret;
}

static void busefb_remove(struct spi_device *spi)
{
	struct busefb_par *par = spi_get_drvdata(spi);

	hrtimer_cancel(&par->cs_delay_timer);
	unregister_framebuffer(par->info);
	destroy_workqueue(par->wq);
	vfree(par->shadow_vram);
	vfree(par->info->screen_base);
	framebuffer_release(par->info);
}

static const struct of_device_id busefb_of_match[] = {
	{ .compatible = "buse,buse128x19" },
	{ }
};
MODULE_DEVICE_TABLE(of, busefb_of_match);

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

