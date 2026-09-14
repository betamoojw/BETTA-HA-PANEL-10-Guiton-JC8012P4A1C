/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_screensaver.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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

#define SCREENSAVER_BG_HEX 0x000000
#define SCREENSAVER_ACCENT_HEX 0xFFFFFF
#define SCREENSAVER_POLL_MS 250

/* Wallpaper is uploaded from the web editor and stored on the SD card. */
#define SCREENSAVER_WALLPAPER_DIR  "/sdcard/bg"
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
    /* Path the decoded wallpaper_dsc was loaded from, so we can reuse the
     * decoded RGB565 across screensaver show/hide cycles and only re-decode
     * when the resolved candidate changes. */
    char wallpaper_path[APP_MAX_IMAGE_PATH_LEN];
} screensaver_state_t;

static screensaver_state_t s_ss = {0};

static void screensaver_release_wallpaper(void)
{
    if (s_ss.wallpaper_owned && s_ss.wallpaper_dsc.data != NULL) {
        heap_caps_free((void *)s_ss.wallpaper_dsc.data);
    }
    memset(&s_ss.wallpaper_dsc, 0, sizeof(s_ss.wallpaper_dsc));
    s_ss.wallpaper_owned = false;
    s_ss.wallpaper_path[0] = '\0';
}

static void screensaver_hide(void)
{
    if (s_ss.root != NULL) {
        lv_obj_del(s_ss.root);
    }
    s_ss.root = NULL;
    s_ss.clock_label = NULL;
    s_ss.visible = false;
    /* Keep the decoded wallpaper so the next show doesn't have to re-run the
     * (expensive) PNG decode while camera/HA may have consumed PSRAM. */
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

#define SCREENSAVER_MAX_CANDIDATES 16

/* Build the ordered list of wallpaper candidates for the screensaver:
 *   1. the user's explicit selection,
 *   2. the default /sdcard/bg/screensaver.png when present,
 *   3. every other .png in /sdcard/bg/, sorted alphabetically for a
 *      deterministic fallback.
 *
 * Each entry is a full /sdcard/bg/... path and duplicates are dropped. The
 * list lets the caller try candidates in order until one actually decodes
 * (e.g. skip an oversized screensaver.png and use the next PNG on the card),
 * so the screensaver works right after a fresh flash with no manual upload.
 * Returns the number of candidates written to `paths`. */
static int screensaver_collect_candidates(char paths[][APP_MAX_IMAGE_PATH_LEN],
                                          int max_candidates,
                                          const display_power_config_t *cfg)
{
    int count = 0;

    if (cfg->screensaver_wallpaper[0] != '\0' && count < max_candidates) {
        (void)snprintf(paths[count], APP_MAX_IMAGE_PATH_LEN, "%s/%s",
                       SCREENSAVER_WALLPAPER_DIR, cfg->screensaver_wallpaper);
        count++;
    }

    struct stat st;
    if (count < max_candidates &&
        stat(SCREENSAVER_WALLPAPER_PATH, &st) == 0 && S_ISREG(st.st_mode)) {
        bool dup = false;
        for (int i = 0; i < count; i++) {
            if (strcmp(paths[i], SCREENSAVER_WALLPAPER_PATH) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            strlcpy(paths[count], SCREENSAVER_WALLPAPER_PATH, APP_MAX_IMAGE_PATH_LEN);
            count++;
        }
    }

    /* Remaining candidates start here; sorted below for a stable fallback. */
    int first_discovered = count;

    DIR *d = opendir(SCREENSAVER_WALLPAPER_DIR);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count < max_candidates) {
            const char *name = e->d_name;
            size_t n = strlen(name);
            if (n < 5 || strcasecmp(name + n - 4, ".png") != 0) {
                continue;
            }
            if (e->d_type == DT_DIR) {
                continue;
            }
            char full[288]; /* dir + '/' + NAME_MAX + NUL */
            (void)snprintf(full, sizeof(full), "%s/%s", SCREENSAVER_WALLPAPER_DIR, name);
            if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
                continue;
            }
            bool dup = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(paths[i], full) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            strlcpy(paths[count], full, APP_MAX_IMAGE_PATH_LEN);
            count++;
        }
        closedir(d);
    }

    /* Sort only the discovered tail; explicit selection and screensaver.png
     * keep their priority. */
    for (int i = first_discovered; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(paths[j], paths[i]) < 0) {
                char tmp[APP_MAX_IMAGE_PATH_LEN];
                strlcpy(tmp, paths[i], sizeof(tmp));
                strlcpy(paths[i], paths[j], APP_MAX_IMAGE_PATH_LEN);
                strlcpy(paths[j], tmp, APP_MAX_IMAGE_PATH_LEN);
            }
        }
    }

    return count;
}

/* Decode (or reuse) the best available wallpaper into s_ss.wallpaper_dsc.
 * Returns true when a decoded RGB565 image is available in s_ss.wallpaper_dsc.
 * Called once at init (while PSRAM is still plentiful, before the camera and
 * HA tasks allocate) and again from screensaver_show() when the cached image
 * is missing or the resolved candidate changed. */
static bool screensaver_load_wallpaper(const display_power_config_t *cfg)
{
    char wallpaper_paths[SCREENSAVER_MAX_CANDIDATES][APP_MAX_IMAGE_PATH_LEN] = {{0}};
    int candidate_count = screensaver_collect_candidates(wallpaper_paths,
                                                         SCREENSAVER_MAX_CANDIDATES, cfg);
    if (candidate_count == 0) {
        ESP_LOGI(TAG_UI, "Screensaver wallpaper: none found on SD card");
        return false;
    }

    /* Reuse the cached decode when the preferred candidate hasn't changed. */
    if (s_ss.wallpaper_owned && s_ss.wallpaper_dsc.data != NULL &&
        s_ss.wallpaper_path[0] != '\0' &&
        strcmp(s_ss.wallpaper_path, wallpaper_paths[0]) == 0) {
        return true;
    }

    screensaver_release_wallpaper();

    for (int i = 0; i < candidate_count; i++) {
        ESP_LOGI(TAG_UI, "Screensaver wallpaper: trying %s", wallpaper_paths[i]);
        if (!ui_image_load_png_file(wallpaper_paths[i], APP_SCREEN_WIDTH,
                                    APP_SCREEN_HEIGHT, &s_ss.wallpaper_dsc)) {
            continue;
        }
        strlcpy(s_ss.wallpaper_path, wallpaper_paths[i], sizeof(s_ss.wallpaper_path));
        s_ss.wallpaper_owned = true;
        return true;
    }
    return false;
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

    display_power_config_t cfg;
    display_get_power_config(&cfg);

    /* Prefer the user-selected wallpaper from the SD card; when none was
     * chosen (fresh flash) auto-discover a PNG already on the card so the
     * screensaver works right after flashing without any manual upload or
     * selection. Fall back to the compile-time embedded emblem when the
     * card/file is missing.
     *
     * IMPORTANT: the image widget stores the dsc pointer (LV_IMAGE_SRC_VARIABLE)
     * without copying it, so the decoded image lives in the persistent
     * s_ss.wallpaper_dsc for as long as the screensaver may need it. */
    bool have_image = screensaver_load_wallpaper(&cfg);
    if (have_image) {
        lv_obj_t *img = lv_image_create(root);
        lv_image_set_src(img, &s_ss.wallpaper_dsc);
        lv_obj_set_size(img, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
        lv_obj_set_pos(img, 0, 0);
    } else {
        lv_obj_t *fallback = lv_label_create(root);
        lv_label_set_text(fallback, "BETTA");
        lv_obj_set_style_text_color(fallback, lv_color_hex(SCREENSAVER_ACCENT_HEX), LV_PART_MAIN);
        lv_obj_set_style_text_font(fallback, APP_FONT_DISPLAY_34, LV_PART_MAIN);
        lv_obj_center(fallback);
    }

    s_ss.root = root;
    s_ss.visible = true;

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

    /* Decode the wallpaper now, while boot has not yet started the camera and
     * HA tasks: PSRAM is still at its post-boot maximum, so a large PNG fits
     * here even though the same decode would fail once the panel is under
     * load. The decoded RGB565 stays cached for every later screensaver
     * cycle. */
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    if (display_lock(5000)) {
        (void)screensaver_load_wallpaper(&cfg);
        display_unlock();
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
    screensaver_release_wallpaper();
    display_unlock();
}
