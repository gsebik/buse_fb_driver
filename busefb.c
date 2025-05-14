/*
 * busefb_fb_driver.c
 * SPI framebuffer driver with hrtimer-based scheduling and manual GPIO CS control.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fb.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#define WIDTH               128
#define HEIGHT               19
#define PANELS                4
#define PANEL_COLS         (WIDTH / PANELS)
#define GROUPS                4
#define COLS_PER_GROUP     (PANEL_COLS / GROUPS)
#define REGS_PER_COL          3
#define DATA_BYTES_PER_PANEL  (COLS_PER_GROUP * REGS_PER_COL)
#define PANEL_BYTES         (DATA_BYTES_PER_PANEL + 1)
#define GROUP_BYTES        (PANELS * PANEL_BYTES)
#define FRAME_BYTES         (GROUPS * GROUP_BYTES)

#define REFRESH_INTERVAL_NS (1000000000 / 120)


struct busefb_par {
    struct spi_device *spi;
    struct fb_info *info;
    struct workqueue_struct *wq;
    struct work_struct refresh_work;
    struct gpio_desc *cs_gpio;
    struct hrtimer refresh_timer;
};

static struct busefb_par *global_par;

static void refresh_work_func(struct work_struct *work)
{
    struct busefb_par *par = global_par;
    u8 frame[FRAME_BYTES] = {0};
    u8 *vram = par->info->screen_base;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int idx = y * WIDTH + x;
            if (!(vram[idx >> 3] & (1 << (idx & 7))))
                continue;

            int y_rev = HEIGHT - 1 - y;
            int reg = y_rev / 8;
            int bit = 7 - (y_rev % 8);
            int panel = x / PANEL_COLS;
            int grp = x % GROUPS;
            int cp = (x % PANEL_COLS) / GROUPS ^ 0x01;
            int base = grp * GROUP_BYTES + panel * PANEL_BYTES;
            frame[base] = grp;
            frame[base + 1 + cp * REGS_PER_COL + reg] |= 1 << bit;
        }
    }

    for (int g = 0; g < GROUPS; g++) {
        gpiod_set_value_cansleep(par->cs_gpio, 1);

        struct spi_transfer t = {
            .tx_buf    = frame + g * GROUP_BYTES,
            .len       = GROUP_BYTES,
            .speed_hz  = par->spi->max_speed_hz,
        };

        struct spi_message m;
        spi_message_init(&m);
        spi_message_add_tail(&t, &m);
        spi_sync(par->spi, &m);

        gpiod_set_value_cansleep(par->cs_gpio, 0);
        udelay(50);
        gpiod_set_value_cansleep(par->cs_gpio, 1);
	udelay(5);
    }
}

static enum hrtimer_restart refresh_timer_callback(struct hrtimer *timer)
{
    struct busefb_par *par = container_of(timer, struct busefb_par, refresh_timer);
    queue_work(par->wq, &par->refresh_work);
    hrtimer_forward_now(timer, ns_to_ktime(REFRESH_INTERVAL_NS));
    return HRTIMER_RESTART;
}

static const struct fb_ops busefb_fbops = {
    .owner = THIS_MODULE,
    .fb_read = fb_sys_read,
    .fb_write = fb_sys_write,
    .fb_fillrect = cfb_fillrect,
    .fb_copyarea = cfb_copyarea,
    .fb_imageblit = cfb_imageblit,
};

static const struct fb_var_screeninfo busefb_screen_info_var = {
    .bits_per_pixel = 1,
};

static int busefb_probe(struct spi_device *spi)
{
    struct busefb_par *par;
    struct fb_info *info;
    int ret;

    par = devm_kzalloc(&spi->dev, sizeof(*par), GFP_KERNEL);
    if (!par)
        return -ENOMEM;

    par->cs_gpio = devm_gpiod_get(&spi->dev, "cs", GPIOD_OUT_HIGH);
    if (IS_ERR(par->cs_gpio))
        return PTR_ERR(par->cs_gpio);

    par->spi = spi;
    global_par = par;

    info = framebuffer_alloc(0, &spi->dev);
    if (!info)
        return -ENOMEM;

    par->info = info;
    info->par = par;
    info->fbops = &busefb_fbops;
    info->fix.smem_len = WIDTH * HEIGHT / 8;
    info->fix.line_length = WIDTH / 8;
    info->var = busefb_screen_info_var;
    info->var.xres = WIDTH;
    info->var.yres = HEIGHT;
    info->var.xres_virtual = WIDTH;
    info->var.yres_virtual = HEIGHT;

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

    INIT_WORK(&par->refresh_work, refresh_work_func);
    hrtimer_init(&par->refresh_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    par->refresh_timer.function = refresh_timer_callback;
    hrtimer_start(&par->refresh_timer, ns_to_ktime(REFRESH_INTERVAL_NS), HRTIMER_MODE_REL);

    ret = register_framebuffer(info);
    if (ret < 0)
        goto err_wq;

    spi_set_drvdata(spi, par);
    dev_info(&spi->dev, "busefb registered as /dev/fb%d\n", info->node);
    return 0;

err_wq:
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
    hrtimer_cancel(&par->refresh_timer);
    unregister_framebuffer(par->info);
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
        .name = "busefb",
        .of_match_table = busefb_of_match,
    },
    .probe = busefb_probe,
    .remove = busefb_remove,
};
module_spi_driver(busefb_driver);

MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Buse 128×19 SPI framebuffer driver with hrtimer-based scheduling and GPIO CS control");
MODULE_LICENSE("GPL");

