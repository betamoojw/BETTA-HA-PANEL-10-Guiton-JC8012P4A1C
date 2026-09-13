/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * entity_list: stacked list of entities (docker containers, game servers,
 * VMs, services, ...) with a status dot, friendly name and value per row.
 * The entity ids come from def->extra_entity_ids (comma separated).
 */
#include "ui/ui_widget_factory.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "ui/fonts/app_text_fonts.h"
#include "ui/ui_memory.h"
#include "ui/theme/theme_default.h"

#define ENTITY_LIST_DOT_SIZE 10

typedef struct {
    lv_obj_t *card;
    lv_obj_t *title_label;
    uint8_t row_count;
    struct {
        char entity_id[APP_MAX_ENTITY_ID_LEN];
        lv_obj_t *dot;
        lv_obj_t *name_label;
        lv_obj_t *value_label;
    } rows[APP_MAX_ENTITY_LIST_ROWS];
} entity_list_ctx_t;

static bool entity_list_state_is_missing(const char *state_text)
{
    if (state_text == NULL || state_text[0] == '\0') {
        return true;
    }
    return strcmp(state_text, "unavailable") == 0 || strcmp(state_text, "unknown") == 0;
}

static bool entity_list_state_is_offline(const char *state_text)
{
    if (state_text == NULL) {
        return true;
    }
    return strcmp(state_text, "off") == 0 || strcmp(state_text, "offline") == 0 ||
           strcmp(state_text, "stopped") == 0 || strcmp(state_text, "disconnected") == 0 ||
           strcmp(state_text, "down") == 0 || strcmp(state_text, "closed") == 0 ||
           strcmp(state_text, "paused") == 0 || strcmp(state_text, "standby") == 0 ||
           strcmp(state_text, "not_home") == 0 || strcmp(state_text, "idle") == 0;
}

static bool entity_list_state_is_error(const char *state_text)
{
    if (state_text == NULL) {
        return false;
    }
    return strcmp(state_text, "error") == 0 || strcmp(state_text, "fault") == 0 ||
           strcmp(state_text, "critical") == 0;
}

static bool entity_list_state_is_warning(const char *state_text)
{
    if (state_text == NULL) {
        return false;
    }
    return strcmp(state_text, "warning") == 0 || strcmp(state_text, "degraded") == 0;
}

static lv_color_t entity_list_state_color(const char *state_text)
{
    if (entity_list_state_is_missing(state_text) || entity_list_state_is_offline(state_text)) {
        return lv_color_hex(APP_UI_COLOR_CARD_BORDER);
    }
    if (entity_list_state_is_error(state_text)) {
        return lv_color_hex(APP_UI_COLOR_ERROR);
    }
    if (entity_list_state_is_warning(state_text)) {
        return lv_color_hex(0xF0A030);
    }
    return lv_color_hex(APP_UI_COLOR_STATE_ON);
}

static bool entity_list_parse_float(const char *text, float *out)
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

static void entity_list_format_state(const ha_state_t *state, char *buf, size_t len)
{
    if (state == NULL || buf == NULL || len == 0) {
        return;
    }
    if (entity_list_state_is_missing(state->state)) {
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
    if (entity_list_parse_float(state->state, &value) && unit != NULL && unit[0] != '\0') {
        snprintf(buf, len, "%s %s", state->state, unit);
    } else {
        snprintf(buf, len, "%s", state->state);
    }

    if (attrs != NULL) {
        cJSON_Delete(attrs);
    }
}

static void entity_list_friendly_name(const char *entity_id, const ha_state_t *state, char *buf, size_t len)
{
    if (state != NULL) {
        cJSON *attrs = cJSON_Parse(state->attributes_json);
        if (attrs != NULL) {
            cJSON *fn = cJSON_GetObjectItemCaseSensitive(attrs, "friendly_name");
            if (cJSON_IsString(fn) && fn->valuestring != NULL && fn->valuestring[0] != '\0') {
                snprintf(buf, len, "%s", fn->valuestring);
                cJSON_Delete(attrs);
                return;
            }
            cJSON_Delete(attrs);
        }
    }

    /* Fallback: entity id suffix with underscores replaced by spaces. */
    const char *suffix = entity_id != NULL ? strchr(entity_id, '.') : NULL;
    if (suffix != NULL) {
        suffix++;
    } else {
        suffix = (entity_id != NULL) ? entity_id : "";
    }

    size_t i = 0;
    while (*suffix != '\0' && i + 1 < len) {
        buf[i++] = (*suffix == '_') ? ' ' : *suffix;
        suffix++;
    }
    buf[i] = '\0';
}

static int entity_list_parse_ids(const char *list, char out[][APP_MAX_ENTITY_ID_LEN], size_t max_rows)
{
    if (list == NULL || list[0] == '\0' || out == NULL || max_rows == 0) {
        return 0;
    }

    size_t count = 0;
    const char *cursor = list;
    while (*cursor != '\0' && count < max_rows) {
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ',') {
            cursor++;
        }
        size_t id_len = (size_t)(cursor - start);
        while (id_len > 0 && (start[id_len - 1] == ' ' || start[id_len - 1] == '\t' ||
                              start[id_len - 1] == '\n' || start[id_len - 1] == '\r')) {
            id_len--;
        }
        if (id_len == 0 || id_len >= APP_MAX_ENTITY_ID_LEN) {
            continue;
        }
        memcpy(out[count], start, id_len);
        out[count][id_len] = '\0';
        count++;
    }
    return (int)count;
}

static bool entity_list_entity_matches(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return strncmp(a, b, APP_MAX_ENTITY_ID_LEN) == 0;
}

static void entity_list_apply_layout(entity_list_ctx_t *ctx)
{
    if (ctx == NULL || ctx->card == NULL || ctx->title_label == NULL || ctx->row_count == 0) {
        return;
    }

    lv_obj_t *card = ctx->card;
    lv_obj_update_layout(card);

    lv_coord_t cw = lv_obj_get_width(card) - lv_obj_get_style_pad_left(card, LV_PART_MAIN) -
                    lv_obj_get_style_pad_right(card, LV_PART_MAIN);
    lv_coord_t ch = lv_obj_get_height(card) - lv_obj_get_style_pad_top(card, LV_PART_MAIN) -
                    lv_obj_get_style_pad_bottom(card, LV_PART_MAIN);
    if (cw < 60) {
        cw = 60;
    }
    if (ch < 60) {
        ch = 60;
    }

    lv_obj_set_style_text_font(ctx->title_label, APP_FONT_TEXT_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(ctx->title_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_width(ctx->title_label, cw);
    lv_obj_set_pos(ctx->title_label, 0, 0);
    lv_obj_update_layout(ctx->title_label);
    lv_coord_t title_h = lv_obj_get_height(ctx->title_label);
    if (title_h < 20) {
        title_h = 20;
    }

    lv_coord_t list_h = ch - title_h - 4;
    if (list_h < ctx->row_count * 12) {
        list_h = ctx->row_count * 12;
    }
    lv_coord_t row_h = list_h / ctx->row_count;
    if (row_h > 32) {
        row_h = 32;
    }
    lv_coord_t dot_size = ENTITY_LIST_DOT_SIZE;
    if (dot_size > row_h - 2) {
        dot_size = (row_h - 2 > 4) ? row_h - 2 : 4;
    }

    lv_coord_t name_w = (cw > 160) ? cw - 90 : cw / 2;

    for (uint8_t i = 0; i < ctx->row_count; i++) {
        lv_coord_t y = title_h + 4 + i * row_h;
        if (ctx->rows[i].dot != NULL) {
            lv_obj_set_size(ctx->rows[i].dot, dot_size, dot_size);
            lv_obj_set_pos(ctx->rows[i].dot, 0, y + (row_h - dot_size) / 2);
        }
        if (ctx->rows[i].name_label != NULL) {
            lv_obj_set_style_text_font(ctx->rows[i].name_label, APP_FONT_TEXT_14, LV_PART_MAIN);
            lv_obj_set_style_text_align(ctx->rows[i].name_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
            lv_obj_set_size(ctx->rows[i].name_label, name_w, row_h);
            lv_obj_set_pos(ctx->rows[i].name_label, dot_size + 6, y);
        }
        if (ctx->rows[i].value_label != NULL) {
            lv_obj_set_style_text_font(ctx->rows[i].value_label, APP_FONT_TEXT_14, LV_PART_MAIN);
            lv_obj_set_style_text_align(ctx->rows[i].value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
            lv_obj_set_size(ctx->rows[i].value_label, cw - name_w - dot_size - 6, row_h);
            lv_obj_set_pos(ctx->rows[i].value_label, name_w + dot_size + 6, y);
        }
    }
}

static void entity_list_mark_all_unavailable(entity_list_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    for (uint8_t i = 0; i < ctx->row_count; i++) {
        if (ctx->rows[i].dot != NULL) {
            lv_obj_set_style_bg_color(
                ctx->rows[i].dot, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);
        }
        if (ctx->rows[i].value_label != NULL) {
            lv_label_set_text(ctx->rows[i].value_label, "--");
        }
    }
}

static void entity_list_event_cb(lv_event_t *event)
{
    if (event == NULL) {
        return;
    }
    entity_list_ctx_t *ctx = (entity_list_ctx_t *)lv_event_get_user_data(event);
    if (ctx == NULL) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        free(ctx);
    } else if (code == LV_EVENT_SIZE_CHANGED) {
        entity_list_apply_layout(ctx);
    }
}

esp_err_t w_entity_list_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
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

    entity_list_ctx_t *ctx = ui_calloc_prefer_psram(1, sizeof(entity_list_ctx_t));
    if (ctx == NULL) {
        lv_obj_del(card);
        return ESP_ERR_NO_MEM;
    }

    ctx->card = card;
    ctx->title_label = title;

    char ids[APP_MAX_ENTITY_LIST_ROWS][APP_MAX_ENTITY_ID_LEN];
    ctx->row_count = (uint8_t)entity_list_parse_ids(def->extra_entity_ids, ids, APP_MAX_ENTITY_LIST_ROWS);

    for (uint8_t i = 0; i < ctx->row_count; i++) {
        snprintf(ctx->rows[i].entity_id, sizeof(ctx->rows[i].entity_id), "%s", ids[i]);

        ctx->rows[i].dot = lv_obj_create(card);
        lv_obj_clear_flag(ctx->rows[i].dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(ctx->rows[i].dot, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(ctx->rows[i].dot, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(ctx->rows[i].dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(ctx->rows[i].dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ctx->rows[i].dot, lv_color_hex(APP_UI_COLOR_CARD_BORDER), LV_PART_MAIN);

        ctx->rows[i].name_label = lv_label_create(card);
        lv_obj_set_style_text_color(ctx->rows[i].name_label, theme_default_color_text_primary(), LV_PART_MAIN);
        lv_label_set_text(ctx->rows[i].name_label, "");

        ctx->rows[i].value_label = lv_label_create(card);
        lv_obj_set_style_text_color(ctx->rows[i].value_label, theme_default_color_text_muted(), LV_PART_MAIN);
        lv_label_set_text(ctx->rows[i].value_label, "--");
    }

    lv_obj_add_event_cb(card, entity_list_event_cb, LV_EVENT_DELETE, ctx);
    lv_obj_add_event_cb(card, entity_list_event_cb, LV_EVENT_SIZE_CHANGED, ctx);

    entity_list_apply_layout(ctx);

    out_instance->obj = card;
    out_instance->ctx = ctx;
    return ESP_OK;
}

void w_entity_list_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }

    entity_list_ctx_t *ctx = (entity_list_ctx_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    for (uint8_t i = 0; i < ctx->row_count; i++) {
        if (!entity_list_entity_matches(ctx->rows[i].entity_id, state->entity_id)) {
            continue;
        }

        if (ctx->rows[i].dot != NULL) {
            lv_obj_set_style_bg_color(ctx->rows[i].dot, entity_list_state_color(state->state), LV_PART_MAIN);
        }
        if (ctx->rows[i].name_label != NULL) {
            char name[APP_MAX_NAME_LEN] = {0};
            entity_list_friendly_name(ctx->rows[i].entity_id, state, name, sizeof(name));
            lv_label_set_text(ctx->rows[i].name_label, name);
        }
        if (ctx->rows[i].value_label != NULL) {
            char value_text[96] = {0};
            entity_list_format_state(state, value_text, sizeof(value_text));
            lv_label_set_text(ctx->rows[i].value_label, value_text);
        }
        return;
    }
}

void w_entity_list_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }

    entity_list_ctx_t *ctx = (entity_list_ctx_t *)instance->ctx;
    if (ctx == NULL) {
        return;
    }

    entity_list_mark_all_unavailable(ctx);
}
