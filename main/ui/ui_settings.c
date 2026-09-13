/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_settings.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app_config.h"
#include "drivers/board_extras_panel10jc.h"
#include "drivers/display_init.h"
#include "net/wifi_mgr.h"
#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_pages.h"
#include "ui/theme/theme_default.h"
#include "drivers/touch_init.h"
#include "settings/runtime_settings.h"
#if CONFIG_APP_FEATURE_XIAOZHI
#include "xiaozhi/xiaozhi_audio.h"
#endif
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
#include "api/api_routes.h"
#include "camera/local_camera.h"
#endif

typedef enum {
    SET_CAT_SCREEN = 0,
#if CONFIG_APP_FEATURE_XIAOZHI
    SET_CAT_AUDIO,
#endif
    SET_CAT_NET,
    SET_CAT_SYSTEM,
    SET_CAT_SD,
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    SET_CAT_CAMERA,
#endif
    SET_CAT_COUNT
} ui_settings_cat_t;

static const char *const s_cat_names[SET_CAT_COUNT] = {
    "Ekran",
#if CONFIG_APP_FEATURE_XIAOZHI
    "Audio",
#endif
    "Siec", "System", "SD",
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    "Kamera",
#endif
};

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_rail = NULL;
static lv_obj_t *s_content = NULL;
static lv_obj_t *s_chips[SET_CAT_COUNT] = {0};
static lv_obj_t *s_chip_labels[SET_CAT_COUNT] = {0};
static ui_settings_cat_t s_cat = SET_CAT_SCREEN;
static bool s_open = false;
static bool s_small = false;

/* ------------------------------------------------------------------ */
/* Slider bookkeeping. Only one content build is alive at a time, so a */
/* small static pool is enough; entries are re-used on every rebuild.  */
/* ------------------------------------------------------------------ */
typedef enum { SET_SL_ACTIVE, SET_SL_DIM, SET_SL_SCREENSAVER, SET_SL_VOLUME } ui_slider_kind_t;
typedef struct {
    ui_slider_kind_t kind;
    lv_obj_t *slider;
    lv_obj_t *value_label;
} ui_slider_ctx_t;

static ui_slider_ctx_t s_sliders[4];
static uint8_t s_slider_count = 0;
static ui_slider_ctx_t *s_active_slider = NULL;

/* Cached copy of the persisted runtime settings, used by the on-panel
 * touch-test toggle (same pattern as the camera settings below). */
static runtime_settings_t s_ui_runtime;
static bool s_ui_runtime_loaded = false;

static void st_rebuild_content(void);

/* ------------------------------------------------------------------ */
/* Geometry / fonts                                                    */
/* ------------------------------------------------------------------ */
static lv_coord_t st_hdr_h(void)
{
    return s_small ? 52 : 80;
}

static lv_coord_t st_rail_w(void)
{
    return s_small ? 150 : 250;
}

static const lv_font_t *st_f_title(void)
{
    return s_small ? APP_FONT_TEXT_22 : APP_FONT_TEXT_28;
}

static const lv_font_t *st_f_cat(void)
{
    return s_small ? APP_FONT_TEXT_16 : APP_FONT_TEXT_20;
}

static const lv_font_t *st_f_row(void)
{
    return s_small ? APP_FONT_TEXT_16 : APP_FONT_TEXT_20;
}

static const lv_font_t *st_f_value(void)
{
    return s_small ? APP_FONT_TEXT_14 : APP_FONT_TEXT_20;
}

static const lv_font_t *st_f_note(void)
{
    return s_small ? APP_FONT_TEXT_12 : APP_FONT_TEXT_16;
}

static void st_label(lv_obj_t *label, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
}

/* ------------------------------------------------------------------ */
/* Content primitives                                                  */
/* ------------------------------------------------------------------ */
static lv_obj_t *st_make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    theme_default_style_card(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *st_card_title(lv_obj_t *card, const char *text)
{
    lv_obj_t *title = lv_label_create(card);
    st_label(title, text, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    return title;
}

/* A two-column row (label left / value right). Returns the value label. */
static lv_obj_t *st_info_row(lv_obj_t *card, const char *label_text, const char *value_text)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    st_label(label, label_text, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(label, LV_PCT(46));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);

    lv_obj_t *value = lv_label_create(row);
    st_label(value, value_text, st_f_value(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_obj_set_width(value, LV_PCT(52));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    return value;
}

static lv_obj_t *st_action_button(lv_obj_t *parent, const char *text, lv_color_t bg)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_width(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_left(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_right(btn, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_top(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(APP_UI_COLOR_CARD_BG_ON), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(btn);
    st_label(label, text, st_f_row(), lv_color_white());
    lv_obj_center(label);
    return btn;
}

/* Flexible spacer: absorbs all remaining vertical space in a flex column so
 * following widgets are pushed to the bottom of the content panel. Used to
 * keep interactive widgets clear of the panel's dead touch band (LVGL y
 * 263..367). */
static void st_add_grow_spacer(lv_obj_t *parent)
{
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, LV_PCT(100));
    lv_obj_set_height(spacer, 0);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
}

/* ------------------------------------------------------------------ */
/* Slider block: caption row + % value + slider                        */
/* ------------------------------------------------------------------ */
static void st_slider_value_changed_cb(lv_event_t *event)
{
    ui_slider_ctx_t *ctx = (ui_slider_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->value_label == NULL) {
        return;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)lv_slider_get_value(ctx->slider));
    lv_label_set_text(ctx->value_label, buf);
}

static void st_slider_release_cb(lv_event_t *event)
{
    ui_slider_ctx_t *ctx = (ui_slider_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL || ctx->slider == NULL) {
        return;
    }
    const int value = (int)lv_slider_get_value(ctx->slider);

    switch (ctx->kind) {
#if CONFIG_APP_FEATURE_XIAOZHI
    case SET_SL_VOLUME:
        (void)xz_audio_set_volume(value);
        break;
#endif
    case SET_SL_ACTIVE:
    case SET_SL_DIM:
    case SET_SL_SCREENSAVER:
    default: {
        display_power_config_t cfg;
        display_get_power_config(&cfg);
        if (ctx->kind == SET_SL_ACTIVE) {
            cfg.active_brightness_percent = value;
        } else if (ctx->kind == SET_SL_DIM) {
            cfg.dim_brightness_percent = value;
        } else {
            cfg.screensaver_brightness_percent = value;
        }
        display_set_power_config(&cfg);
        break;
    }
    }
}

static void st_add_slider_block(lv_obj_t *card, const char *caption, int value, ui_slider_kind_t kind)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(68));
    lv_label_set_long_mode(caption_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *value_label = lv_label_create(row);
    st_label(value_label, "", st_f_value(), lv_color_hex(APP_UI_COLOR_STATE_ON));
    lv_obj_set_width(value_label, LV_PCT(30));
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    if (s_slider_count >= (sizeof(s_sliders) / sizeof(s_sliders[0]))) {
        return;
    }
    ui_slider_ctx_t *ctx = &s_sliders[s_slider_count++];
    ctx->kind = kind;

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 28);
    int slider_max = 100;
#if CONFIG_APP_FEATURE_XIAOZHI
    if (kind == SET_SL_VOLUME) {
        slider_max = xz_audio_get_max_volume();
    }
#endif
    lv_slider_set_range(slider, 0, slider_max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_TRACK_OFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_TRACK_ON), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(APP_UI_COLOR_LIGHT_KNOB_ON), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);

    ctx->slider = slider;
    ctx->value_label = value_label;

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", value);
    lv_label_set_text(value_label, buf);

    lv_obj_add_event_cb(slider, st_slider_value_changed_cb, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(slider, st_slider_release_cb, LV_EVENT_RELEASED, ctx);

    if (kind == SET_SL_ACTIVE) {
        s_active_slider = ctx;
    }
}

/* ------------------------------------------------------------------ */
/* Brightness presets                                                  */
/* ------------------------------------------------------------------ */
static void st_preset_click_cb(lv_event_t *event)
{
    const int value = (int)(uintptr_t)lv_event_get_user_data(event);
    if (s_active_slider == NULL || s_active_slider->slider == NULL) {
        return;
    }
    lv_slider_set_value(s_active_slider->slider, value, LV_ANIM_OFF);

    display_power_config_t cfg;
    display_get_power_config(&cfg);
    cfg.active_brightness_percent = value;
    display_set_power_config(&cfg);

    if (s_active_slider->value_label != NULL) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d%%", value);
        lv_label_set_text(s_active_slider->value_label, buf);
    }
}

static void st_add_preset_row(lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(row, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hint = lv_label_create(row);
    st_label(hint, "Skroty:", st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));

    const int presets[] = {10, 25, 50, 100};
    for (size_t i = 0; i < (sizeof(presets) / sizeof(presets[0])); i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", presets[i]);
        lv_obj_t *btn = st_action_button(row, buf, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
        lv_obj_set_style_pad_left(btn, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_right(btn, 16, LV_PART_MAIN);
        lv_obj_set_style_pad_top(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(btn, 6, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
        lv_obj_set_style_text_color(lv_obj_get_child(btn, 0), lv_color_white(), LV_PART_MAIN);
        lv_obj_add_event_cb(btn, st_preset_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)presets[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Dim timeout dropdown                                                */
/* ------------------------------------------------------------------ */
#define SET_DIM_TIMEOUT_OPTIONS "Nigdy\n30 s\n1 min\n3 min\n5 min\n10 min"
static const uint32_t s_dim_timeout_ms[] = {0U, 30000U, 60000U, 180000U, 300000U, 600000U};

static void st_dropdown_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    if (sel < 0 || sel >= (int)(sizeof(s_dim_timeout_ms) / sizeof(s_dim_timeout_ms[0]))) {
        return;
    }
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    cfg.dim_timeout_ms = s_dim_timeout_ms[sel];
    display_set_power_config(&cfg);
}

static int st_dim_timeout_index(uint32_t ms)
{
    const int count = (int)(sizeof(s_dim_timeout_ms) / sizeof(s_dim_timeout_ms[0]));
    int best = 0;
    uint32_t best_delta = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t delta = (ms > s_dim_timeout_ms[i]) ? (ms - s_dim_timeout_ms[i]) : (s_dim_timeout_ms[i] - ms);
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return best;
}

static void st_add_dim_timeout_dropdown(lv_obj_t *card)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, "Czas wygaszenia", st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));
    lv_label_set_long_mode(caption_label, LV_LABEL_LONG_WRAP);

    display_power_config_t cfg;
    display_get_power_config(&cfg);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, SET_DIM_TIMEOUT_OPTIONS);
    lv_dropdown_set_selected(dropdown, st_dim_timeout_index(cfg.dim_timeout_ms));
    lv_obj_set_width(dropdown, LV_PCT(42));
    lv_obj_set_height(dropdown, 40);
    lv_obj_set_style_text_font(dropdown, st_f_value(), LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dropdown, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(dropdown, st_dropdown_change_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ------------------------------------------------------------------ */
/* Off timeout dropdown + night mode                                    */
/* ------------------------------------------------------------------ */
#define SET_OFF_TIMEOUT_OPTIONS "Nigdy\n15 min\n30 min\n1 godz\n2 godz\n4 godz"
static const uint32_t s_off_timeout_ms[] = {0U, 900000U, 1800000U, 3600000U, 7200000U, 14400000U};

static int st_off_timeout_index(uint32_t ms)
{
    const int count = (int)(sizeof(s_off_timeout_ms) / sizeof(s_off_timeout_ms[0]));
    int best = 0;
    uint32_t best_delta = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t delta = (ms > s_off_timeout_ms[i]) ? (ms - s_off_timeout_ms[i]) : (s_off_timeout_ms[i] - ms);
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return best;
}

static void st_off_timeout_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    if (sel < 0 || sel >= (int)(sizeof(s_off_timeout_ms) / sizeof(s_off_timeout_ms[0]))) {
        return;
    }
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    cfg.off_timeout_ms = s_off_timeout_ms[sel];
    display_set_power_config(&cfg);
}

/* Shared dropdown row builder (caption left / dropdown right). */
static lv_obj_t *st_add_dropdown_row(lv_obj_t *card, const char *caption, const char *options,
    int selected, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));
    lv_label_set_long_mode(caption_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_width(dropdown, LV_PCT(42));
    lv_obj_set_height(dropdown, 40);
    lv_obj_set_style_text_font(dropdown, st_f_value(), LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY), LV_PART_MAIN);
    lv_obj_set_style_border_width(dropdown, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dropdown, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_left(dropdown, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(dropdown, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(dropdown, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return dropdown;
}

static void st_add_off_timeout_dropdown(lv_obj_t *card)
{
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    (void)st_add_dropdown_row(card, "Wylacz po bezczynnosci", SET_OFF_TIMEOUT_OPTIONS,
        st_off_timeout_index(cfg.off_timeout_ms), st_off_timeout_change_cb, NULL);
}

static const char *st_hour_options(void)
{
    static char buf[128];
    if (buf[0] != '\0') {
        return buf;
    }
    size_t off = 0;
    for (int i = 0; i < 24; i++) {
        int n = snprintf(buf + off, sizeof(buf) - off, (i == 23) ? "%d" : "%d\n", i);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
        if (off >= sizeof(buf)) {
            break;
        }
    }
    return buf;
}

static void st_night_hour_change_cb(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target(event);
    const int sel = lv_dropdown_get_selected(dropdown);
    if (sel < 0 || sel > 23) {
        return;
    }
    const bool is_end = (lv_event_get_user_data(event) != NULL);
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    if (is_end) {
        cfg.night_end_hour = sel;
    } else {
        cfg.night_start_hour = sel;
    }
    display_set_power_config(&cfg);
}

static void st_night_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    cfg.night_mode_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    display_set_power_config(&cfg);
}

static void st_add_night_switch_row(lv_obj_t *card)
{
    display_power_config_t cfg;
    display_get_power_config(&cfg);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, "Tryb nocny", st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (cfg.night_mode_enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, st_night_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
static void st_screensaver_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    cfg.screensaver_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    display_set_power_config(&cfg);
}

static void st_add_screensaver_switch_row(lv_obj_t *card)
{
    display_power_config_t cfg;
    display_get_power_config(&cfg);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, "Wygaszacz graficzny", st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (cfg.screensaver_enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, st_screensaver_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void st_screensaver_clock_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    display_power_config_t cfg;
    display_get_power_config(&cfg);
    cfg.screensaver_clock_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    display_set_power_config(&cfg);
}

static void st_add_screensaver_clock_switch_row(lv_obj_t *card)
{
    display_power_config_t cfg;
    display_get_power_config(&cfg);

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, "Zegar na wygaszaczu", st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (cfg.screensaver_clock_enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, st_screensaver_clock_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
#endif

/* ------------------------------------------------------------------ */
/* Touch test switch (red-dot overlay for dead-zone checking)          */
/* ------------------------------------------------------------------ */
static void st_touch_runtime_ensure_loaded(void)
{
    if (s_ui_runtime_loaded) {
        return;
    }
    runtime_settings_set_defaults(&s_ui_runtime);
    (void)runtime_settings_load(&s_ui_runtime);
    s_ui_runtime_loaded = true;
}

static void st_touch_test_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_touch_runtime_ensure_loaded();
    s_ui_runtime.touch_test = on;
    touch_debug_set_enabled(on);
    (void)runtime_settings_save(&s_ui_runtime);
}

static void st_add_touch_test_switch_row(lv_obj_t *card)
{
    st_touch_runtime_ensure_loaded();

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, "Tester dotyku (kropki)", st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));
    lv_label_set_long_mode(caption_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (s_ui_runtime.touch_test) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, st_touch_test_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ------------------------------------------------------------------ */
/* Category content builders                                           */
/* ------------------------------------------------------------------ */
static void st_build_screen(lv_obj_t *parent)
{
    display_power_config_t cfg;
    display_get_power_config(&cfg);

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Jasnosc");
    st_add_slider_block(card, "Jasnosc aktywna", cfg.active_brightness_percent, SET_SL_ACTIVE);
    st_add_preset_row(card);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Wygaszanie");
    st_add_slider_block(card2, "Jasnosc po bezczynnosci", cfg.dim_brightness_percent, SET_SL_DIM);
    st_add_dim_timeout_dropdown(card2);
    st_add_off_timeout_dropdown(card2);

    lv_obj_t *card3 = st_make_card(parent);
    st_card_title(card3, "Tryb nocny");
    st_add_night_switch_row(card3);
    (void)st_add_dropdown_row(card3, "Poczatek (godz)", st_hour_options(),
        cfg.night_start_hour, st_night_hour_change_cb, NULL);
    (void)st_add_dropdown_row(card3, "Koniec (godz)", st_hour_options(),
        cfg.night_end_hour, st_night_hour_change_cb, (void *)(uintptr_t)1);

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC
    lv_obj_t *card_ss = st_make_card(parent);
    st_card_title(card_ss, "Wygaszacz");
    st_add_screensaver_switch_row(card_ss);
    st_add_screensaver_clock_switch_row(card_ss);
    st_add_slider_block(card_ss, "Jasnosc wygaszacza", cfg.screensaver_brightness_percent, SET_SL_SCREENSAVER);
#endif

    lv_obj_t *card4 = st_make_card(parent);
    st_card_title(card4, "Uwaga");
    lv_obj_t *note = lv_label_create(card4);
    st_label(note, "Ekran wygasza sie po okresie bez dotyku, a po dluzszym czasie calkowicie sie wylacza. "
        "W nocy ekran wylacza sie od razu po wygaszeniu. Dotkniecie ekranu przywraca jasnosc.",
        st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
}

#if CONFIG_APP_FEATURE_XIAOZHI
static void st_beep_evt_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI("UI_SET", "Beep PRESSED");
    } else if (code == LV_EVENT_RELEASED) {
        ESP_LOGI("UI_SET", "Beep RELEASED");
    } else if (code == LV_EVENT_CLICKED) {
        ESP_LOGI("UI_SET", "Beep CLICKED");
        (void)xz_audio_test_beep();
    }
}

static void st_build_audio(lv_obj_t *parent)
{
    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Glosnosc");
    st_add_slider_block(card, "Poziom glosnosci", xz_audio_get_volume(), SET_SL_VOLUME);

    /* Keep the "Test" card clear of the dead touch band by pinning it to the
     * bottom of the content panel (see ANALIZA-NAPRAWA-DOTYKU.md). */
    st_add_grow_spacer(parent);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Test");
    lv_obj_t *beep = st_action_button(card2, "Odtworz test", lv_color_hex(APP_UI_COLOR_OK));
    lv_obj_set_width(beep, LV_PCT(100));
    lv_obj_set_height(beep, 64);
    lv_obj_add_event_cb(beep, st_beep_evt_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(beep, st_beep_evt_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(beep, st_beep_evt_cb, LV_EVENT_CLICKED, NULL);
    ESP_LOGI("UI_SET", "Audio page built");
}
#endif

static void st_net_refresh_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    if (s_cat == SET_CAT_NET) {
        st_rebuild_content();
    }
}

static void st_build_net(lv_obj_t *parent)
{
    bool connected = wifi_mgr_is_connected();
    bool setup_ap = wifi_mgr_is_setup_ap_active();

    char ssid[APP_WIFI_SSID_MAX_LEN] = "-";
    int8_t rssi = 0;
    wifi_mgr_sta_ap_info_t ap = {0};
    if (wifi_mgr_get_sta_ap_info(&ap) == ESP_OK && ap.ssid[0] != '\0') {
        snprintf(ssid, sizeof(ssid), "%s", ap.ssid);
        rssi = ap.rssi;
    }

    char ip[64] = "-";
    (void)wifi_mgr_get_sta_ip(ip, sizeof(ip));

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Status polaczenia");

    const char *status_text = "Brak polaczenia";
    lv_color_t status_color = lv_color_hex(APP_UI_COLOR_ERROR);
    if (setup_ap) {
        status_text = "Tryb AP (konfiguracja)";
        status_color = lv_color_hex(APP_UI_COLOR_STATE_ON);
    } else if (connected) {
        status_text = "Polaczono";
        status_color = lv_color_hex(APP_UI_COLOR_OK);
    }

    lv_obj_t *status_value = st_info_row(card, "Stan", status_text);
    st_label(status_value, status_text, st_f_value(), status_color);

    st_info_row(card, "SSID", ssid);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d dBm", (int)rssi);
        st_info_row(card, "Sygnal", buf);
    }
    st_info_row(card, "IP", ip);

    lv_obj_t *card2 = st_make_card(parent);
    lv_obj_t *refresh = st_action_button(card2, "Odswiez", lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
    lv_obj_add_event_cb(refresh, st_net_refresh_cb, LV_EVENT_CLICKED, NULL);
}

static void st_restart_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    esp_restart();
}

static void st_reset_display_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    display_power_config_t cfg;
    cfg.active_brightness_percent = APP_DISPLAY_ACTIVE_BRIGHTNESS_PERCENT;
    cfg.dim_brightness_percent = APP_DISPLAY_DIM_BRIGHTNESS_PERCENT;
    cfg.dim_timeout_ms = APP_DISPLAY_DIM_TIMEOUT_MS;
    display_set_power_config(&cfg);
}

static void st_build_system(lv_obj_t *parent)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    char lvgl_ver[32];
    snprintf(lvgl_ver, sizeof(lvgl_ver), "%d.%d.%d", (int)LVGL_VERSION_MAJOR, (int)LVGL_VERSION_MINOR,
        (int)LVGL_VERSION_PATCH);

    char mac[20];
    {
        uint8_t raw[6];
        esp_read_mac(raw, ESP_MAC_BASE);
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    }

    int64_t uptime_secs = esp_timer_get_time() / 1000000LL;
    char uptime[64];
    {
        int64_t days = uptime_secs / 86400;
        int64_t rem = uptime_secs % 86400;
        int h = (int)(rem / 3600);
        int m = (int)((rem % 3600) / 60);
        int sec = (int)(rem % 60);
        snprintf(uptime, sizeof(uptime), "%lldd %02d:%02d:%02d", (long long)days, h, m, sec);
    }

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    char psram[40];
    snprintf(psram, sizeof(psram), "%.1f / %.1f MB", (double)psram_free / (1024.0 * 1024.0),
        (double)psram_total / (1024.0 * 1024.0));
    char ram[40];
    snprintf(ram, sizeof(ram), "%.1f MB wolne", (double)ram_free / (1024.0 * 1024.0));

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Informacje");

    char buf[96];
    snprintf(buf, sizeof(buf), "%s (%s)", desc->version, desc->project_name);
    st_info_row(card, "Firmware", buf);
    st_info_row(card, "IDF", esp_get_idf_version());
    st_info_row(card, "LVGL", lvgl_ver);
    st_info_row(card, "MAC", mac);
    st_info_row(card, "Dzialanie", uptime);
    st_info_row(card, "PSRAM", psram);
    st_info_row(card, "RAM", ram);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Akcje");
    lv_obj_t *btn_row = lv_obj_create(card2);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reset = st_action_button(btn_row, "Przywroc ekran", lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE));
    lv_obj_add_event_cb(reset, st_reset_display_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *restart = st_action_button(btn_row, "Restart urzadzenia", lv_color_hex(APP_UI_COLOR_ERROR));
    lv_obj_add_event_cb(restart, st_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card_touch = st_make_card(parent);
    st_card_title(card_touch, "Dotyk");
    st_add_touch_test_switch_row(card_touch);
}

static void st_build_sd(lv_obj_t *parent)
{
    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Karta SD");

    if (sdcard_is_ready()) {
        uint64_t total = 0;
        uint64_t free_bytes = 0;
        bool have_info = (sdcard_info(&total, &free_bytes) == ESP_OK);

        st_info_row(card, "Status", "Zamontowana");

        if (have_info) {
            char total_str[32];
            char free_str[32];
            snprintf(total_str, sizeof(total_str), "%.1f GB",
                     (double)total / (1024.0 * 1024.0 * 1024.0));
            snprintf(free_str, sizeof(free_str), "%.1f GB",
                     (double)free_bytes / (1024.0 * 1024.0 * 1024.0));
            st_info_row(card, "Pojemnosc", total_str);
            st_info_row(card, "Wolne", free_str);
        }

        lv_obj_t *note = lv_label_create(card);
        st_label(note, "Karta jest zamontowana w /sdcard i gotowa do zapisu danych "
                       "diagnostycznych oraz dziennikow.",
            st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, LV_PCT(100));
    } else {
        lv_obj_t *note = lv_label_create(card);
        st_label(note, "Nie wykryto karty SD.", st_f_row(),
            lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note, LV_PCT(100));

        lv_obj_t *note2 = lv_label_create(card);
        st_label(note2, "Wloz karte microSD i zrestartuj urzadzenie. Karta jest "
                        "montowana automatycznie podczas uruchamiania (FAT32/FAT, /sdcard).",
            st_f_note(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
        lv_label_set_long_mode(note2, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(note2, LV_PCT(100));
    }
}

#if CONFIG_APP_FEATURE_LOCAL_CAMERA
/* Camera settings mirror the persisted runtime settings; switches apply
 * immediately to the live camera pipeline AND are written back to flash. */
static runtime_settings_t s_cam_cfg;
static bool s_cam_cfg_loaded = false;

static void st_camera_ensure_loaded(void)
{
    if (s_cam_cfg_loaded) {
        return;
    }
    runtime_settings_set_defaults(&s_cam_cfg);
    (void)runtime_settings_load(&s_cam_cfg);
    s_cam_cfg_loaded = true;
}

static void st_camera_persist(void)
{
    st_camera_ensure_loaded();
    (void)runtime_settings_save(&s_cam_cfg);
}

static void st_camera_apply_runtime(void)
{
    /* Re-apply the static camera parameters (motion/flip/quality) so a
     * freshly started pipeline keeps the user's choices. */
    local_camera_set_motion_wake(s_cam_cfg.camera_motion_wake);
    local_camera_set_motion_threshold((uint8_t)s_cam_cfg.camera_motion_threshold);
    local_camera_set_jpeg_quality((uint8_t)s_cam_cfg.camera_jpeg_quality);
    local_camera_set_flip(s_cam_cfg.camera_hflip, s_cam_cfg.camera_vflip);
}

static void st_camera_motion_wake_cb(void *user_data)
{
    (void)user_data;
    display_note_activity();
}

static void st_camera_stream_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_camera_ensure_loaded();
    s_cam_cfg.camera_stream_enabled = on;
    api_camera_local_set_stream_enabled(on);
    st_camera_persist();
}

static void st_camera_enabled_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_camera_ensure_loaded();
    s_cam_cfg.camera_enabled = on;
    if (on) {
        if (local_camera_start() == ESP_OK) {
            local_camera_register_motion_cb(st_camera_motion_wake_cb, NULL);
            st_camera_apply_runtime();
        }
    } else {
        local_camera_stop();
    }
    st_camera_persist();
}

static void st_camera_motion_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_camera_ensure_loaded();
    s_cam_cfg.camera_motion_wake = on;
    local_camera_set_motion_wake(on);
    st_camera_persist();
}

static void st_camera_hflip_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_camera_ensure_loaded();
    s_cam_cfg.camera_hflip = on;
    local_camera_set_flip(s_cam_cfg.camera_hflip, s_cam_cfg.camera_vflip);
    st_camera_persist();
}

static void st_camera_vflip_switch_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    const bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    st_camera_ensure_loaded();
    s_cam_cfg.camera_vflip = on;
    local_camera_set_flip(s_cam_cfg.camera_hflip, s_cam_cfg.camera_vflip);
    st_camera_persist();
}

static lv_obj_t *st_add_camera_switch_row(lv_obj_t *card, const char *caption, bool initial,
                                          lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *caption_label = lv_label_create(row);
    st_label(caption_label, caption, st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_obj_set_width(caption_label, LV_PCT(55));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_width(sw, 64);
    lv_obj_set_height(sw, 34);
    if (initial) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void st_build_camera(lv_obj_t *parent)
{
    st_camera_ensure_loaded();

    lv_obj_t *card = st_make_card(parent);
    st_card_title(card, "Kamera");

    const bool running = local_camera_is_running();
    const bool stream_on = api_camera_local_get_stream_enabled();

    st_add_camera_switch_row(card, "Kamera wlaczona", running, st_camera_enabled_switch_cb);
    st_add_camera_switch_row(card, "Transmisja na zywo (HA)", stream_on, st_camera_stream_switch_cb);
    st_add_camera_switch_row(card, "Wykrywanie ruchu", s_cam_cfg.camera_motion_wake,
                             st_camera_motion_switch_cb);
    st_add_camera_switch_row(card, "Odbicie poziome (H)", s_cam_cfg.camera_hflip,
                             st_camera_hflip_switch_cb);
    st_add_camera_switch_row(card, "Odbicie pionowe (V)", s_cam_cfg.camera_vflip,
                             st_camera_vflip_switch_cb);

    char info[128];
    snprintf(info, sizeof(info), "Rozdzielczosc: %d x %d", local_camera_width(), local_camera_height());
    st_info_row(card, "Rozdzielczosc", info);

    lv_obj_t *card2 = st_make_card(parent);
    st_card_title(card2, "Transmisja do Home Assistant");
    lv_obj_t *note = lv_label_create(card2);
    st_label(note,
        "Wlacz transmisje, a nastepnie dodaj w HA kamere MJPEG z adresem: "
        "http://IP_PANELU/api/camera/stream",
        st_f_row(), lv_color_hex(APP_UI_COLOR_TEXT_PRIMARY));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
}
#endif

/* ------------------------------------------------------------------ */
/* Content / category rendering                                        */
/* ------------------------------------------------------------------ */
static void st_style_chips(void)
{
    for (int i = 0; i < SET_CAT_COUNT; i++) {
        lv_obj_t *chip = s_chips[i];
        lv_obj_t *label = s_chip_labels[i];
        if (chip == NULL || label == NULL) {
            continue;
        }
        bool active = (i == (int)s_cat);
        if (active) {
            lv_obj_set_style_bg_color(chip, lv_color_hex(APP_UI_COLOR_NAV_BTN_BG_ACTIVE), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_NAV_TAB_ACTIVE), LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_color(chip, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(APP_UI_COLOR_TEXT_MUTED), LV_PART_MAIN);
        }
    }
}

static void st_chip_click_cb(lv_event_t *event)
{
    const int idx = (int)(uintptr_t)lv_event_get_user_data(event);
    if (idx < 0 || idx >= SET_CAT_COUNT || idx == (int)s_cat) {
        return;
    }
    s_cat = (ui_settings_cat_t)idx;
    ESP_LOGI("UI_SET", "Category selected: %d", idx);
    st_style_chips();
    st_rebuild_content();
}

static void st_rebuild_content(void)
{
    if (s_content == NULL) {
        return;
    }
    s_slider_count = 0;
    s_active_slider = NULL;

    lv_obj_clean(s_content);
    lv_obj_scroll_to_y(s_content, 0, LV_ANIM_OFF);

    switch (s_cat) {
#if CONFIG_APP_FEATURE_XIAOZHI
    case SET_CAT_AUDIO:
        st_build_audio(s_content);
        break;
#endif
    case SET_CAT_NET:
        st_build_net(s_content);
        break;
    case SET_CAT_SYSTEM:
        st_build_system(s_content);
        break;
    case SET_CAT_SD:
        st_build_sd(s_content);
        break;
#if CONFIG_APP_FEATURE_LOCAL_CAMERA
    case SET_CAT_CAMERA:
        st_build_camera(s_content);
        break;
#endif
    case SET_CAT_SCREEN:
    default:
        st_build_screen(s_content);
        break;
    }
}

static void st_close_cb(lv_event_t *event)
{
    LV_UNUSED(event);
    if (s_overlay != NULL) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    s_rail = NULL;
    s_content = NULL;
    s_open = false;
    s_active_slider = NULL;
    s_slider_count = 0;
}

/* ------------------------------------------------------------------ */
/* Overlay creation                                                    */
/* ------------------------------------------------------------------ */
static void st_open(void)
{
    if (s_open || s_overlay != NULL) {
        return;
    }
    s_open = true;
    s_small = (APP_SCREEN_WIDTH < 700);
    s_cat = SET_CAT_SCREEN;
    s_slider_count = 0;
    s_active_slider = NULL;
    memset(s_chips, 0, sizeof(s_chips));
    memset(s_chip_labels, 0, sizeof(s_chip_labels));

    lv_obj_t *screen = lv_scr_act();
    s_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, APP_SCREEN_WIDTH, APP_SCREEN_HEIGHT);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_overlay);

    /* Header */
    lv_obj_t *header = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, APP_SCREEN_WIDTH, st_hdr_h());
    lv_obj_set_pos(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(header, lv_color_hex(APP_UI_COLOR_TOPBAR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(APP_UI_COLOR_TOPBAR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(header, LV_OPA_80, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(header);
    st_label(title, "Ustawienia", st_f_title(), lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    lv_obj_t *close = lv_obj_create(header);
    lv_obj_remove_style_all(close);
    lv_coord_t close_size = s_small ? 44 : 56;
    lv_obj_set_size(close, close_size, close_size);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_radius(close, close_size / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(close, lv_color_hex(APP_UI_COLOR_TOPBAR_CHIP_BG), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(close, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(close, st_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = lv_label_create(close);
    st_label(close_label, LV_SYMBOL_CLOSE, st_f_title(), lv_color_hex(APP_UI_COLOR_TOPBAR_TEXT));
    lv_obj_center(close_label);

    /* Left category rail */
    lv_obj_t *rail = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(rail);
    lv_obj_set_size(rail, st_rail_w(), APP_SCREEN_HEIGHT - st_hdr_h());
    lv_obj_set_pos(rail, 0, st_hdr_h());
    lv_obj_set_style_bg_color(rail, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(rail, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_color(rail, lv_color_hex(APP_UI_COLOR_CONTENT_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(rail, 10, LV_PART_MAIN);
    /* No flex layout: chips are placed at explicit 2-column coordinates below.
     * Flex ROW_WRAP rendered single-column on this LVGL build, which pushed the
     * SD chip into the dead touch band (LVGL y ~253..371) and made it
     * untappable. Explicit positions guarantee 2 columns and keep every chip
     * above that band. */
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    s_rail = rail;

    const lv_coord_t rail_pad = 10;
    const lv_coord_t gap_x = 8;
    const lv_coord_t gap_y = 6;
    const lv_coord_t chip_h = s_small ? 36 : 44;
    const lv_coord_t chip_w = (st_rail_w() - 2 * rail_pad - gap_x) / 2;

    for (int i = 0; i < SET_CAT_COUNT; i++) {
        lv_obj_t *chip = lv_obj_create(rail);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, chip_w, chip_h);
        lv_obj_set_pos(chip,
                       rail_pad + (i % 2) * (chip_w + gap_x),
                       rail_pad + (i / 2) * (chip_h + gap_y));
        lv_obj_set_style_radius(chip, 14, LV_PART_MAIN);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(chip, st_chip_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(chip);
        st_label(label, s_cat_names[i], st_f_cat(), lv_color_hex(APP_UI_COLOR_TEXT_MUTED));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

        s_chips[i] = chip;
        s_chip_labels[i] = label;
    }
    st_style_chips();

    /* Right content panel */
    lv_obj_t *content = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, APP_SCREEN_WIDTH - st_rail_w(), APP_SCREEN_HEIGHT - st_hdr_h());
    lv_obj_set_pos(content, st_rail_w(), st_hdr_h());
    lv_obj_set_style_pad_all(content, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 14, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(content, lv_color_hex(APP_UI_COLOR_CONTENT_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_ON);
    lv_obj_set_style_pad_right(content, 26, LV_PART_MAIN);
    s_content = content;

    st_rebuild_content();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
static void st_gear_cb(void)
{
    if (s_open || s_overlay != NULL) {
        st_close_cb(NULL);
    } else {
        st_open();
    }
}

static void st_screen_built_cb(void)
{
    /* ui_pages_init() cleaned the active screen: any overlay we owned is gone. */
    s_overlay = NULL;
    s_rail = NULL;
    s_content = NULL;
    s_open = false;
    s_active_slider = NULL;
    s_slider_count = 0;
    memset(s_chips, 0, sizeof(s_chips));
    memset(s_chip_labels, 0, sizeof(s_chip_labels));
}

void ui_settings_init(void)
{
    ui_pages_set_gear_callback(st_gear_cb);
    ui_pages_set_screen_built_callback(st_screen_built_cb);
}

void ui_settings_toggle(void)
{
    st_gear_cb();
}

bool ui_settings_is_open(void)
{
    return s_open;
}
