/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * temp_tile: dedicated temperature tile.
 * Renders a single temperature sensor as either a ring gauge (graphic mode)
 * or a big plain number (text mode). A thermometer icon plus the editable
 * title label sit in the centre, the value + unit below.
 */
#include "ui/ui_widget_factory.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "esp_log.h"

#include "ui/fonts/app_text_fonts.h"
#include "ui/fonts/mdi_font_registry.h"
#include "ui/ui_memory.h"
#include "ui/theme/theme_default.h"

/* mdi-thermometer */
#define TEMP_ICON_CP 0xF0531U

static const char *TAG = "temp_tile";

#if LV_FONT_MONTSERRAT_44
#define TEMP_VALUE_FONT_LARGE (&lv_font_montserrat_44)
#elif LV_FONT_MONTSERRAT_40
#define TEMP_VALUE_FONT_LARGE (&lv_font_montserrat_40)
#elif LV_FONT_MONTSERRAT_36
#define TEMP_VALUE_FONT_LARGE (&lv_font_montserrat_36)
#elif LV_FONT_MONTSERRAT_34
#define TEMP_VALUE_FONT_LARGE APP_FONT_TEXT_34
#else
#define TEMP_VALUE_FONT_LARGE APP_FONT_DISPLAY_34
#endif

#if LV_FONT_MONTSERRAT_28
#define TEMP_VALUE_FONT_MEDIUM APP_FONT_TEXT_28
#elif LV_FONT_MONTSERRAT_24
#define TEMP_VALUE_FONT_MEDIUM APP_FONT_TEXT_24
#else
#define TEMP_VALUE_FONT_MEDIUM APP_FONT_TEXT_20
#endif

typedef struct {
    lv_obj_t *card;
    lv_obj_t *title_label;
    lv_obj_t *icon_label;
    lv_obj_t *value_label;
    lv_obj_t *arc;
    bool graphic;
    int min;
    int max;
    bool unavailable;
} temp_ctx_t;

static bool temp_state_is_unavailable(const char *state_text)
{
    if (state_text == NULL || state_text[0] == '\0') {
        return true;
    }
    return strcmp(state_text, "unavailable") == 0 || strcmp(state_text, "unknown") == 0;
}

static bool temp_parse_float(const char *text, float *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    char *end = NULL;
    float v = strtof(text, &end);
    if (end == text) {
        return false;
    }
    *out = v;
    return true;
}

static bool temp_unit_is_percent(const char *unit)
{
    return unit != NULL && strcmp(unit, "%") == 0;
}

/* Hardcoded temperature scale colors so the arc is readable regardless of
 * the active theme (theme `state_on` can be white/yellow and is not a
 * meaningful temperature color). */
#define TEMP_COLOR_BLUE   0x42A5F5U
#define TEMP_COLOR_GREEN  0x4CAF50U
#define TEMP_COLOR_ORANGE 0xFF9800U
#define TEMP_COLOR_RED    0xE53935U

static lv_color_t temp_threshold_color(float value, const char *unit)
{
    if (temp_unit_is_percent(unit)) {
        if (value >= 85.0f) {
            return lv_color_hex(TEMP_COLOR_RED);
        }
        if (value >= 60.0f) {
            return lv_color_hex(TEMP_COLOR_ORANGE);
        }
        if (value >= 30.0f) {
            return lv_color_hex(TEMP_COLOR_GREEN);
        }
        return lv_color_hex(TEMP_COLOR_BLUE);
    }
    /* Temperature thresholds: blue < 35 C, green < 60 C, orange < 75 C, red >= 75 C. */
    if (value < 35.0f) {
        return lv_color_hex(TEMP_COLOR_BLUE);
    }
    if (value < 60.0f) {
        return lv_color_hex(TEMP_COLOR_GREEN);
    }
    if (value < 75.0f) {
        return lv_color_hex(TEMP_COLOR_ORANGE);
    }
    return lv_color_hex(TEMP_COLOR_RED);
}

static void temp_arc_anim_exec_cb(void *var, int32_t value)
{
    lv_obj_t *arc = (lv_obj_t *)var;
    if (arc != NULL && lv_obj_is_valid(arc)) {
        lv_arc_set_value(arc, value);
    }
}

static void temp_arc_set_value_anim(lv_obj_t *arc, int32_t target)
{
    if (arc == NULL) {
        return;
    }

    int32_t current = (int32_t)lv_arc_get_value(arc);
    if (current == target) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, temp_arc_anim_exec_cb);
    lv_anim_set_values(&a, current, target);
    lv_anim_set_duration(&a, 600);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void temp_apply_arc(temp_ctx_t *ctx, float value, const char *unit)
{
    if (ctx == NULL || ctx->arc == NULL) {
        return;
    }

    int lo = (ctx->max > ctx->min) ? ctx->min : 0;
    int hi = (ctx->max > ctx->min) ? ctx->max : 100;

    lv_arc_set_range(ctx->arc, lo, hi);
    int v = (int)(value + 0.5f);
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    temp_arc_set_value_anim(ctx->arc, v);
    lv_obj_set_style_arc_color(ctx->arc, temp_threshold_color(value, unit), LV_PART_INDICATOR);
}

static bool temp_font_has_icon(const lv_font_t *font)
{
    if (font == NULL) {
        return false;
    }
    lv_font_glyph_dsc_t dsc = {0};
    return lv_font_get_glyph_dsc(font, &dsc, TEMP_ICON_CP, 0);
}

static const char *temp_icon_utf8_from_codepoint(uint32_t codepoint)
{
    static char utf8[5] = {0};

    if (codepoint <= 0x7FU) {
        utf8[0] = (char)codepoint;
        utf8[1] = '\0';
    } else if (codepoint <= 0x7FFU) {
        utf8[0] = (char)(0xC0U | ((codepoint >> 6) & 0x1FU));
        utf8[1] = (char)(0x80U | (codepoint & 0x3FU));
        utf8[2] = '\0';
    } else if (codepoint <= 0xFFFFU) {
        utf8[0] = (char)(0xE0U | ((codepoint >> 12) & 0x0FU));
        utf8[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        utf8[2] = (char)(0x80U | (codepoint & 0x3FU));
        utf8[3] = '\0';
    } else {
        utf8[0] = (char)(0xF0U | ((codepoint >> 18) & 0x07U));
        utf8[1] = (char)(0x80U | ((codepoint >> 12) & 0x3FU));
        utf8[2] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        utf8[3] = (char)(0x80U | (codepoint & 0x3FU));
        utf8[4] = '\0';
    }
    return utf8;
}

static const lv_font_t *temp_icon_font_for_min_dim(lv_coord_t min_dim)
{
    const lv_font_t *font = NULL;
    if (min_dim >= 300) {
        font = mdi_font_icon_72();
    } else if (min_dim >= 200) {
        font = mdi_font_icon_56();
    } else {
        font = mdi_font_icon_42();
    }
    if (!temp_font_has_icon(font)) {
        font = mdi_font_large();
    }
    if (temp_font_has_icon(font)) {
        return font;
    }
    return LV_FONT_DEFAULT;
}

static const char *temp_icon_text_for_font(const lv_font_t *font)
{
    if (font != NULL && temp_font_has_icon(font)) {
        return temp_icon_utf8_from_codepoint(TEMP_ICON_CP);
    }
    return "";
}

static const lv_font_t *temp_pick_value_font(const temp_ctx_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL) {
        return TEMP_VALUE_FONT_MEDIUM;
    }
    lv_coord_t w = lv_obj_get_width(ctx->card);
    lv_coord_t h = lv_obj_get_height(ctx->card);
    lv_coord_t min_dim = (w < h) ? w : h;

    if (min_dim >= 220) {
        return TEMP_VALUE_FONT_LARGE;
    }
    if (min_dim >= 150) {
        return TEMP_VALUE_FONT_MEDIUM;
    }
    return APP_FONT_TEXT_20;
}

static void temp_format_state(const ha_state_t *state, char *buf, size_t len)
{
    if (state == NULL || buf == NULL || len == 0) {
        return;
    }
    if (temp_state_is_unavailable(state->state)) {
        snprintf(buf, len, "--");
        return;
    }

    const char *unit = NULL;
    cJSON *attrs = cJSON_Parse(state->attributes_json);
    if (attrs != NULL) {
        cJSON *unit_item = cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement");
        if (cJSON_IsString(unit_item) && unit_item->valuestring != NULL) {
            unit = unit_item->valuestring;
        }
    }

    float value = 0.0f;
    bool numeric = temp_parse_float(state->state, &value);
    if (numeric && unit != NULL && unit[0] != '\0') {
        if (temp_unit_is_percent(unit)) {
            snprintf(buf, len, "%.1f%%", value);
        } else {
            snprintf(buf, len, "%.1f %s", value, unit);
        }
    } else {
        snprintf(buf, len, "%s", state->state);
    }

    if (attrs != NULL) {
        cJSON_Delete(attrs);
    }
}

static void temp_set_value_text(temp_ctx_t *ctx, const char *text)
{
    if (ctx == NULL || ctx->value_label == NULL) {
        return;
    }
    lv_label_set_text(ctx->value_label, (text != NULL && text[0] != '\0') ? text : "--");
}

static void temp_apply_layout(temp_ctx_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL || ctx->title_label == NULL || ctx->value_label == NULL) {
        return;
    }

    lv_obj_t *card = ctx->card;
    lv_obj_update_layout(card);

    lv_coord_t cw = lv_obj_get_width(card) - lv_obj_get_style_pad_left(card, LV_PART_MAIN) -
                    lv_obj_get_style_pad_right(card, LV_PART_MAIN);
    lv_coord_t ch = lv_obj_get_height(card) - lv_obj_get_style_pad_top(card, LV_PART_MAIN) -
                    lv_obj_get_style_pad_bottom(card, LV_PART_MAIN);
    if (cw < 40) {
        cw = 40;
    }
    if (ch < 60) {
        ch = 60;
    }

    lv_coord_t value_h = (ch >= 120) ? 44 : 34;
    lv_coord_t value_y = ch - value_h;
    lv_obj_set_style_text_align(ctx->value_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(ctx->value_label, temp_pick_value_font(ctx), LV_PART_MAIN);
    lv_obj_set_size(ctx->value_label, cw, value_h);
    lv_obj_set_pos(ctx->value_label, 0, value_y);

    lv_coord_t icon_w = cw;
    lv_coord_t icon_h = value_y;
    if (icon_h < 30) {
        icon_h = 30;
    }

    if (ctx->graphic && ctx->arc != NULL) {
        lv_coord_t arc_diam = (cw < icon_h) ? cw : icon_h;
        if (arc_diam < 60) {
            arc_diam = 60;
        }
        if (arc_diam > icon_h) {
            arc_diam = icon_h;
        }
        lv_obj_set_size(ctx->arc, arc_diam, arc_diam);
        lv_obj_set_pos(ctx->arc, (cw - arc_diam) / 2, (icon_h - arc_diam) / 2);

        lv_coord_t min_dim = arc_diam;
        const lv_font_t *icon_font = temp_icon_font_for_min_dim(min_dim);
        lv_obj_set_style_text_font(ctx->icon_label, icon_font, LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(ctx->icon_label, cw, arc_diam / 2);
        lv_obj_set_pos(ctx->icon_label, 0, (icon_h - arc_diam) / 2 + arc_diam / 4 - 4);

        lv_obj_set_style_text_font(ctx->title_label, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(ctx->title_label, cw, 22);
        lv_obj_set_pos(ctx->title_label, 0, (icon_h + arc_diam) / 2 - 18);
    } else {
        /* text mode: icon on top, label beneath, value at the bottom */
        const lv_font_t *icon_font = temp_icon_font_for_min_dim((cw < icon_h) ? cw : icon_h);
        lv_obj_set_style_text_font(ctx->icon_label, icon_font, LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->icon_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(ctx->icon_label, cw, (icon_h * 2) / 3);
        lv_obj_set_pos(ctx->icon_label, 0, 0);

        lv_obj_set_style_text_font(ctx->title_label, APP_FONT_TEXT_16, LV_PART_MAIN);
        lv_obj_set_style_text_align(ctx->title_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_size(ctx->title_label, cw, 22);
        lv_obj_set_pos(ctx->title_label, 0, (icon_h * 2) / 3 - 4);
    }

    lv_obj_set_style_text_color(ctx->title_label, theme_default_color_text_muted(), LV_PART_MAIN);
}

static void temp_apply_unavailable(temp_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->unavailable = true;
    temp_set_value_text(ctx, "--");
    if (ctx->arc != NULL) {
        lv_anim_delete(ctx->arc, temp_arc_anim_exec_cb);
        lv_arc_set_value(ctx->arc, 0);
    }
    temp_apply_layout(ctx);
}

static void temp_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }
    temp_ctx_t *ctx = (temp_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        if (ctx->arc != NULL) {
            lv_anim_delete(ctx->arc, temp_arc_anim_exec_cb);
        }
        free(ctx);
    } else if (code == LV_EVENT_SIZE_CHANGED) {
        temp_apply_layout(ctx);
    }
}

esp_err_t w_temp_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, def->x, def->y);
    lv_obj_set_size(card, def->w, def->h);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    theme_default_style_card(card);
    lv_obj_set_style_pad_left(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_top(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(card, 10, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, def->title[0] ? def->title : def->id);
    lv_obj_set_style_text_color(title, theme_default_color_text_muted(), LV_PART_MAIN);

    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, "");
    lv_obj_set_style_text_color(icon, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_MAIN);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_color(value, theme_default_color_text_primary(), LV_PART_MAIN);

    bool graphic = !(def->style_variant[0] != '\0' && strcmp(def->style_variant, "text") == 0);

    lv_obj_t *arc = NULL;
    if (graphic) {
        arc = lv_arc_create(card);
        lv_arc_set_mode(arc, LV_ARC_MODE_NORMAL);
        lv_arc_set_range(arc, 0, 100);
        lv_arc_set_value(arc, 0);
        lv_arc_set_rotation(arc, 0);
        lv_arc_set_bg_angles(arc, 135, 45);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_color(arc, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(APP_UI_COLOR_STATE_ON), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
        lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    }

    temp_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(temp_ctx_t));
    if (ctx == NULL) {
        lv_obj_del(card);
        return ESP_ERR_NO_MEM;
    }

    ctx->card = card;
    ctx->title_label = title;
    ctx->icon_label = icon;
    ctx->value_label = value;
    ctx->arc = arc;
    ctx->graphic = graphic;
    ctx->min = def->sensor_min;
    ctx->max = def->sensor_max;
    ctx->unavailable = false;

    lv_obj_add_event_cb(card, temp_event_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(card, temp_event_cb, LV_EVENT_SIZE_CHANGED, ctx);

    temp_apply_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

static bool temp_entity_matches(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a, b, APP_MAX_ENTITY_ID_LEN) == 0;
}

void w_temp_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }

    temp_ctx_t *ctx = (temp_ctx_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    if (!temp_entity_matches(instance->entity_id, state->entity_id)) {
        return;
    }

    if (temp_state_is_unavailable(state->state)) {
        temp_apply_unavailable(ctx);
        return;
    }

    char value_text[96] = {0};
    temp_format_state(state, value_text, sizeof(value_text));
    ctx->unavailable = false;
    temp_set_value_text(ctx, value_text);

    if (ctx->arc != NULL) {
        float fvalue = 0.0f;
        if (temp_parse_float(state->state, &fvalue)) {
            const char *unit = NULL;
            cJSON *attrs = cJSON_Parse(state->attributes_json);
            if (attrs != NULL) {
                cJSON *unit_item = cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement");
                if (cJSON_IsString(unit_item) && unit_item->valuestring != NULL) {
                    unit = unit_item->valuestring;
                }
            }
            temp_apply_arc(ctx, fvalue, unit);
            ESP_LOGI(TAG, "apply %s value=%.1f unit=%s min=%d max=%d",
                instance->entity_id, (double)fvalue, unit != NULL ? unit : "-", ctx->min, ctx->max);
            if (attrs != NULL) {
                cJSON_Delete(attrs);
            }
        }
    }

    temp_apply_layout(ctx);
}

void w_temp_tile_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }

    temp_ctx_t *ctx = (temp_ctx_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    temp_apply_unavailable(ctx);
}
