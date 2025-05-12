/*
 * busefb_fb_driver.c
 *
 * SPI framebuffer driver for a 128×19 display using 4 panels (32 columns each)
 * and 4 interleaved column groups. Each SPI frame is 4 groups × (4 panels × 25 bytes) = 400 bytes:
 *   [group selector][24 data bytes] × 4 panels per group.
 * The selector byte (0–3) precedes each panel's 24 data bytes.
 * Refresh runs at ~60 Hz via a workqueue.
 * Uses native SPI CE0 inverted (active-high) via SPI_CS_HIGH.
 *
 * Build:
 *   obj-m += busefb_fb_driver.o
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>

#define WIDTH               128
#define HEIGHT               19
#define PANELS                4    /* 32 columns per panel */
#define PANEL_COLS         (WIDTH / PANELS)
#define GROUPS                4    /* 4 interleaved column groups */
#define COLS_PER_GROUP     (PANEL_COLS / GROUPS) /* 8 */
#define REGS_PER_COL          3    /* bytes per column: rows0-7,8-15,16-18 */
#define DATA_BYTES_PER_PANEL  (COLS_PER_GROUP * REGS_PER_COL) /* 24 */
#define PANEL_BYTES         (DATA_BYTES_PER_PANEL + 1) /* +1 selector =25 */
#define GROUP_BYTES        (PANELS * PANEL_BYTES)    /* 100 */
#define FRAME_BYTES         (GROUPS * GROUP_BYTES)    /* 400 */
#define REFRESH_HZ            120

#define PIXEL_CONTENT_SHIFT_DOWN 5

struct busefb_par {
    struct spi_device       *spi;
    struct fb_info          *info;
    struct workqueue_struct *wq;
    struct delayed_work      refresh_work;
};

/*
 * Refresh work: pack VRAM into the SPI frame and transmit.
 */
static void busefb_refresh_work(struct work_struct *work)
{
    struct busefb_par *par =
        container_of(to_delayed_work(work), struct busefb_par, refresh_work);
    u8 frame[FRAME_BYTES];
    u8 *vram = par->info->screen_base;
    int g, p;

    /* Iterate each pixel in VRAM and map to shift-register frame */
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int idx = y * WIDTH + x;
            if (!(vram[idx >> 3] & (1 << (idx & 7))))
                continue;
            /* vertical flip: display row */
            int y_rev = HEIGHT - 1 - y;
            int reg = y_rev / 8;
            int bit = 7-y_rev % 8;
            
	    /* compute panel and group */
            int panel = x / PANEL_COLS;
            int grp = x % GROUPS;

            /* which column within this group's panel */
            int cp = (x % PANEL_COLS) / GROUPS;
	    cp ^= 0x01;
            /* set selector byte */
            int base = grp * GROUP_BYTES + panel * PANEL_BYTES;
            frame[base] = grp;

            /* pack bit */
            int db = cp * REGS_PER_COL;
            frame[base + 1 + db + reg] |= 1 << bit;
        }
    }


    /* Transmit each group via SPI */
    for (g = 0; g < GROUPS; g++) {
        struct spi_transfer t = {
            .tx_buf   = frame + g * GROUP_BYTES,
            .len      = GROUP_BYTES,
            .speed_hz = par->spi->max_speed_hz,
            .cs_change = 0,
        };
        spi_sync_transfer(par->spi, &t, 1);
        udelay(50);  /* latch delay */
    }

    /* Reschedule next refresh */
    queue_delayed_work(par->wq, &par->refresh_work,
                       msecs_to_jiffies(1000 / REFRESH_HZ));
}

/* Standard fbdev operations */
static const struct fb_ops busefb_fbops = {
    .owner        = THIS_MODULE,
    .fb_read      = fb_sys_read,
    .fb_write     = fb_sys_write,
    .fb_fillrect  = cfb_fillrect,
    .fb_copyarea  = cfb_copyarea,
    .fb_imageblit = cfb_imageblit,
};

static int busefb_probe(struct spi_device *spi)
{
    struct busefb_par *par;
    struct fb_info *info;
    int ret;

    /* Use inverted CE0 */
    spi->mode |= SPI_CS_HIGH;
    ret = spi_setup(spi);
    if (ret)
        return ret;

    par = devm_kzalloc(&spi->dev, sizeof(*par), GFP_KERNEL);
    if (!par)
        return -ENOMEM;
    par->spi = spi;

    info = framebuffer_alloc(0, &spi->dev);
    if (!info)
        return -ENOMEM;
    par->info  = info;
    info->par   = par;
    info->fbops = &busefb_fbops;

    /* Configure resolution and depth */
    info->fix.smem_len       = WIDTH * HEIGHT / 8;
    info->fix.line_length    = WIDTH / 8;
    info->var.xres           = WIDTH;
    info->var.yres           = HEIGHT;
    info->var.bits_per_pixel = 1;

    info->screen_base = vzalloc(info->fix.smem_len);
    if (!info->screen_base) {
        ret = -ENOMEM;
        goto err_fb;
    }

    par->wq = create_singlethread_workqueue("busefb_wq");
    if (!par->wq) {
        ret = -ENOMEM;
        goto err_vram;
    }
    INIT_DELAYED_WORK(&par->refresh_work, busefb_refresh_work);
    queue_delayed_work(par->wq, &par->refresh_work, 0);

    ret = register_framebuffer(info);
    if (ret < 0)
        goto err_wq;

    spi_set_drvdata(spi, par);
    dev_info(&spi->dev, "busefb registered as /dev/fb%d\n", info->node);
    return 0;

err_wq:
    cancel_delayed_work_sync(&par->refresh_work);
    destroy_workqueue(par->wq);
err_vram:
    vfree(info->screen_base);
err_fb:
    framebuffer_release(info);
    return ret;
}

static void busefb_remove(struct spi_device *spi)
{
    struct busefb_par *par = spi_get_drvdata(spi);
    unregister_framebuffer(par->info);
    cancel_delayed_work_sync(&par->refresh_work);
    destroy_workqueue(par->wq);
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
        .name           = "busefb",
        .of_match_table = busefb_of_match,
    },
    .probe  = busefb_probe,
    .remove = busefb_remove,
};
module_spi_driver(busefb_driver);

MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Buse 128×19 SPI framebuffer driver");
MODULE_LICENSE("GPL");

