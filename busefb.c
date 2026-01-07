// SPDX-License-Identifier: GPL-2.0
/*
 * Buse SPI framebuffer driver
 * Supports:
 *   - 128x19 (4x32)
 *   - 144x19 (4x32 + 16 half panel at end of chain)
 *
 * Mapping:
 *  - 4 column groups (grp = x % 4)
 *  - FIFO shift-register cascade (panels reversed in buffer)
 *  - Column-pair mirroring within each panel (cp reversed, grp preserved)
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

#define GROUPS 4
#define DISPLAY_BRIGHTNESS_USEC 50

struct busefb_config {
    u32 width;
    u32 height;

    u32 full_panels;     /* number of 32-col panels */
    u32 panel_width;     /* normally 32 */
    u32 tail_width;      /* 0 or 16 */

    u32 regs_per_col;

    u32 cols_per_group_full;
    u32 cols_per_group_tail;

    u32 panel_bytes_full;
    u32 panel_bytes_tail;

    u32 group_bytes;
    u32 frame_bytes;

    /* per-group SPI order:
     * [tail][panel3][panel2][panel1][panel0]
     */
    u32 panel_off[5];
};

struct busefb_par {
    struct spi_device *spi;
    struct fb_info *info;
    struct gpio_desc *cs_gpio;

    struct workqueue_struct *wq;
    struct work_struct refresh_work;
    struct work_struct cs_reassert_work;
    struct hrtimer cs_delay_timer;
    spinlock_t fb_lock;

    struct busefb_config cfg;

    u8 *frame_buffer;
    u8 *shadow_vram;

    int current_group;
};

/* Forward */
static void process_next_group(struct busefb_par *par);

/* ---------- CS timing ---------- */

static void cs_reassert_work_func(struct work_struct *work)
{
    struct busefb_par *par =
        container_of(work, struct busefb_par, cs_reassert_work);

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
    struct busefb_par *par =
        container_of(timer, struct busefb_par, cs_delay_timer);

    queue_work(par->wq, &par->cs_reassert_work);
    return HRTIMER_NORESTART;
}

static void process_next_group(struct busefb_par *par)
{
    struct busefb_config *cfg = &par->cfg;
    int g = par->current_group;

    struct spi_transfer t = {
        .tx_buf = par->frame_buffer + g * cfg->group_bytes,
        .len = cfg->group_bytes,
        .speed_hz = par->spi->max_speed_hz,
    };
    struct spi_message m;

    gpiod_set_value(par->cs_gpio, 1);

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    spi_sync(par->spi, &m);

    gpiod_set_value(par->cs_gpio, 0);

    hrtimer_start(&par->cs_delay_timer,
              ktime_set(0, DISPLAY_BRIGHTNESS_USEC * 1000),
              HRTIMER_MODE_REL);
}

/* ---------- Frame build ---------- */

static void refresh_work_func(struct work_struct *work)
{
    struct busefb_par *par =
        container_of(work, struct busefb_par, refresh_work);
    struct busefb_config *cfg = &par->cfg;
    unsigned long flags;

    spin_lock_irqsave(&par->fb_lock, flags);
    memcpy(par->shadow_vram,
           par->info->screen_base,
           par->info->fix.smem_len);
    memset(par->frame_buffer, 0, cfg->frame_bytes);
    spin_unlock_irqrestore(&par->fb_lock, flags);

    for (int y = 0; y < cfg->height; y++) {
        for (int x = 0; x < cfg->width; x++) {

            int idx = y * cfg->width + x;
            if (!(par->shadow_vram[idx >> 3] & (1 << (idx & 7))))
                continue;

            int grp = x % GROUPS;

            int y_rev = cfg->height - 1 - y;
            int reg = y_rev / 8;
            int bit = 7 - (y_rev % 8);

            int base = grp * cfg->group_bytes;

            /* Tail panel (furthest in chain) */
            if (cfg->tail_width &&
                x >= cfg->full_panels * cfg->panel_width) {

                int x_in = x - cfg->full_panels * cfg->panel_width;
                int cp_raw = x_in / GROUPS;
                /* Mirror column-pair within panel */
                int cp = (cfg->cols_per_group_tail - 1) - cp_raw;

                par->frame_buffer[base] = grp;
                par->frame_buffer[base +
                    1 + cp * cfg->regs_per_col + reg] |=
                    1 << bit;
            } else {
                /* Full panels */
                int p = x / cfg->panel_width;
                int x_in = x - p * cfg->panel_width;
                int cp_raw = x_in / GROUPS;
                /* Mirror column-pair within panel */
                int cp = (cfg->cols_per_group_full - 1) - cp_raw;

                int panel_index =
                    (cfg->tail_width ? 1 : 0) +
                    (cfg->full_panels - 1 - p);

                int off = cfg->panel_off[panel_index];

                par->frame_buffer[base + off] = grp;
                par->frame_buffer[base + off +
                    1 + cp * cfg->regs_per_col + reg] |=
                    1 << bit;
            }
        }
    }

    par->current_group = 0;
    process_next_group(par);
}

/* ---------- FB ops ---------- */

static const struct fb_ops busefb_ops = {
    .owner       = THIS_MODULE,
    .fb_read     = fb_sys_read,
    .fb_write    = fb_sys_write,
    .fb_fillrect = cfb_fillrect,
    .fb_copyarea = cfb_copyarea,
    .fb_imageblit = cfb_imageblit,
};

/* ---------- Probe ---------- */

static int busefb_probe(struct spi_device *spi)
{
    struct fb_info *info;
    struct busefb_par *par;
    struct busefb_config *cfg;
    int ret;

    u32 width = 128, height = 19, panels = 4;
    u32 panel_width = 32, tail_width = 0;

    info = framebuffer_alloc(sizeof(*par), &spi->dev);
    if (!info)
        return -ENOMEM;

    par = info->par;
    par->spi = spi;
    par->info = info;
    cfg = &par->cfg;
    spin_lock_init(&par->fb_lock);

    device_property_read_u32(&spi->dev, "width", &width);
    device_property_read_u32(&spi->dev, "height", &height);
    device_property_read_u32(&spi->dev, "panels", &panels);
    device_property_read_u32(&spi->dev, "panel-width", &panel_width);
    device_property_read_u32(&spi->dev, "tail-width", &tail_width);

    if (!tail_width && width > panels * panel_width)
        tail_width = width - panels * panel_width;

    cfg->width = width;
    cfg->height = height;
    cfg->full_panels = panels;
    cfg->panel_width = panel_width;
    cfg->tail_width = tail_width;

    cfg->regs_per_col = DIV_ROUND_UP(height, 8);
    cfg->cols_per_group_full = panel_width / GROUPS;
    cfg->cols_per_group_tail = tail_width ? tail_width / GROUPS : 0;

    cfg->panel_bytes_full =
        1 + cfg->cols_per_group_full * cfg->regs_per_col;
    cfg->panel_bytes_tail =
        tail_width ? 1 + cfg->cols_per_group_tail * cfg->regs_per_col : 0;

    /* SPI layout: [tail][panel3][panel2][panel1][panel0] */
    u32 off = 0, i = 0;

    if (cfg->tail_width) {
        cfg->panel_off[i++] = off;
        off += cfg->panel_bytes_tail;
    }

    for (int p = cfg->full_panels - 1; p >= 0; p--) {
        cfg->panel_off[i++] = off;
        off += cfg->panel_bytes_full;
    }

    cfg->group_bytes = off;
    cfg->frame_bytes = GROUPS * off;

    par->frame_buffer = vzalloc(cfg->frame_bytes);
    if (!par->frame_buffer) {
        ret = -ENOMEM;
        goto err_release;
    }

    par->cs_gpio = devm_gpiod_get(&spi->dev, "cs", GPIOD_OUT_HIGH);
    if (IS_ERR(par->cs_gpio)) {
        ret = PTR_ERR(par->cs_gpio);
        goto err_free_fb;
    }

    info->fix = (struct fb_fix_screeninfo){
        .id = "busefb",
        .type = FB_TYPE_PACKED_PIXELS,
        .visual = FB_VISUAL_MONO01,
        .line_length = cfg->width / 8,
        .smem_len = (cfg->width * cfg->height) / 8,
    };

    info->var = (struct fb_var_screeninfo){
        .bits_per_pixel = 1,
        .xres = cfg->width,
        .yres = cfg->height,
        .xres_virtual = cfg->width,
        .yres_virtual = cfg->height,
    };

    info->fbops = &busefb_ops;

    info->screen_base = vzalloc(info->fix.smem_len);
    if (!info->screen_base) {
        ret = -ENOMEM;
        goto err_free_fb;
    }

    par->shadow_vram = vzalloc(info->fix.smem_len);
    if (!par->shadow_vram) {
        ret = -ENOMEM;
        goto err_free_screen;
    }

    par->wq = create_singlethread_workqueue("busefb_wq");
    if (!par->wq) {
        ret = -ENOMEM;
        goto err_free_shadow;
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

    dev_info(&spi->dev,
         "busefb: %ux%u (%u full + %u tail)\n",
         cfg->width, cfg->height,
         cfg->full_panels, cfg->tail_width);

    return 0;

err_destroy_wq:
    destroy_workqueue(par->wq);
err_free_shadow:
    vfree(par->shadow_vram);
err_free_screen:
    vfree(info->screen_base);
err_free_fb:
    vfree(par->frame_buffer);
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
    vfree(par->frame_buffer);
    framebuffer_release(par->info);
}

static const struct of_device_id busefb_of_match[] = {
    { .compatible = "buse,busefb" },
    { }
};
MODULE_DEVICE_TABLE(of, busefb_of_match);

static struct spi_driver busefb_driver = {
    .driver = {
        .name = "busefb",
        .of_match_table = busefb_of_match,
    },
    .probe  = busefb_probe,
    .remove = busefb_remove,
};

module_spi_driver(busefb_driver);

MODULE_AUTHOR("You");
MODULE_DESCRIPTION("Buse SPI framebuffer driver (128x19 / 144x19)");
MODULE_LICENSE("GPL");
