/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_screensaver.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "app_config.h"
#include "drivers/display_init.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_i18n.h"
#include "ui/ui_image_loader.h"
#include "util/log_tags.h"

#if defined(__has_include)
#if __has_include("ui/assets/screensaver_image.h")
#include "ui/assets/screensaver_image.h"
#define APP_HAVE_SCREENSAVER_IMAGE 1
#endif
#endif

#ifndef APP_HAVE_SCREENSAVER_IMAGE
#define APP_HAVE_SCREENSAVER_IMAGE 0
#endif

#define SCREENSAVER_BG_HEX 0x000000
#define SCREENSAVER_ACCENT_HEX 0xFFFFFF
#define SCREENSAVER_POLL_MS 250

/* Wallpaper is uploaded from the web editor and stored on the SD card. */
#define SCREENSAVER_WALLPAPER_PATH "/sdcard/bg/screensaver.png"

/* Time before which we consider NTP to be unsynchronized (2021-01-01 UTC). */
#define SCREENSAVER_SYNCED_EPOCH 1609459200

typedef struct {
    lv_obj_t *root;
    lv_obj_t *clock_label;
    lv_timer_t *timer;
    lv_image_dsc_t wallpaper_dsc;
    bool wallpaper_owned;
    bool visible;
} screensaver_state_t;

static screensaver_state_t s_ss = {0};

static void screensaver_release_wallpaper(void)
{
    if (s_ss.wallpaper_owned && s_ss.wallpaper_dsc.data != NULL) {
        heap_caps_free((void *)s_ss.wallpaper_dsc.data);
    }
    memset(&s_ss.wallpaper_dsc, 0, sizeof(s_ss.wallpaper_dsc));
    s_ss.wallpaper_owned = false;
}

static void screensaver_hide(void)
{
    if (s_ss.root != NULL) {
        lv_obj_del(s_ss.root);
    }
    s_ss.root = NULL;
    s_ss.clock_label = NULL;
    s_ss.visible = false;
    screensaver_release_wallpaper();
}

static void screensaver_update_clock(void)
{
    if (s_ss.clock_label == NULL) {
        return;
    }

    char buf[16] = "--:--";
    const time_t now = time(NULL);
    if (now >= SCREENSAVER_SYNCED_EPOCH) {
        struct tm tm_now;
        if (localtime_r(&now, &tm_now) != NULL) {
            snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
        }
    }
    lv_label_set_text(s_ss.clock_label, buf);
}

static void screensaver_touch_cb(lv_event_t *event)
{
    (void)event;
    /* The indev activity handler already notes the touch and restores the
     * active brightness; we just dismiss the overlay. */
    screensaver_hide();
}

static void screensaver_show(void)
{
    if (s_ss.root != NULL) {
        return;
    }

    lv_obj_t *root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(root, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(root, lv_color_hex(SCREENSAVER_BG_HEX), LV_PART_MAIN);
    lv_obj_move_foreground(root);
    lv_obj_add_event_cb(root, screensaver_touch_cb, LV_EVENT_PRESSED, NULL);

    /* Prefer the wallpaper uploaded to the SD card; fall back to the
     * compile-time embedded emblem when the card/file is missing.
     *
     * IMPORTANT: load directly into the persistent s_ss.wallpaper_dsc. The
     * image widget stores the dsc pointer (LV_IMAGE_SRC_VARIABLE) without
     * copying it, so a stack-local dsc would dangle once this function
     * returns and cause a crash/restart on the next redraw. */
    bool have_image = false;
    if (ui_image_load_png_file(SCREENSAVER_WALLPAPER_PATH, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT,
                               &s_ss.wallpaper_dsc)) {
        lv_obj_t *img = lv_image_create(root);
        lv_image_set_src(img, &s_ss.wallpaper_dsc);
        lv_obj_set_size(img, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
        lv_obj_set_pos(img, 0, 0);
        s_ss.wallpaper_owned = true;
        have_image = true;
    }
#if APP_HAVE_SCREENSAVER_IMAGE
    if (!have_image) {
        lv_obj_t *img = lv_image_create(root);
        lv_image_set_src(img, &screensaver_image);
        lv_obj_center(img);
        have_image = true;
    }
#endif
    if (!have_image) {
        lv_obj_t *fallback = lv_label_create(root);
        lv_label_set_text(fallback, "BETTA");
        lv_obj_set_style_text_color(fallback, lv_color_hex(SCREENSAVER_ACCENT_HEX), LV_PART_MAIN);
        lv_obj_set_style_text_font(fallback, APP_FONT_DISPLAY_34, LV_PART_MAIN);
        lv_obj_center(fallback);
    }

    s_ss.root = root;
    s_ss.visible = true;

    display_power_config_t cfg;
    display_get_power_config(&cfg);
    if (cfg.screensaver_clock_enabled) {
        lv_obj_t *clock = lv_label_create(root);
        lv_obj_set_style_text_color(clock, lv_color_hex(SCREENSAVER_ACCENT_HEX), LV_PART_MAIN);
        lv_obj_set_style_text_font(clock, APP_FONT_DISPLAY_34, LV_PART_MAIN);
        lv_obj_set_style_bg_color(clock, lv_color_hex(SCREENSAVER_BG_HEX), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(clock, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_radius(clock, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(clock, 24, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(clock, 10, LV_PART_MAIN);
        lv_obj_align(clock, LV_ALIGN_BOTTOM_MID, 0, -40);
        s_ss.clock_label = clock;
    }

    screensaver_update_clock();
    lv_refr_now(NULL);
}

static void screensaver_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    const bool active = display_power_is_screensaver_active();
    if (active && !s_ss.visible) {
        screensaver_show();
    } else if (!active && s_ss.visible) {
        screensaver_hide();
    } else if (active) {
        screensaver_update_clock();
    }
}

esp_err_t ui_screensaver_init(void)
{
    if (s_ss.timer != NULL) {
        return ESP_OK;
    }
    if (!display_lock(200)) {
        return ESP_ERR_TIMEOUT;
    }
    s_ss.timer = lv_timer_create(screensaver_timer_cb, SCREENSAVER_POLL_MS, NULL);
    display_unlock();
    if (s_ss.timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG_UI, "Graphical screensaver initialized");
    return ESP_OK;
}

void ui_screensaver_deinit(void)
{
    if (!display_lock(200)) {
        return;
    }
    if (s_ss.timer != NULL) {
        lv_timer_del(s_ss.timer);
        s_ss.timer = NULL;
    }
    screensaver_hide();
    display_unlock();
}
