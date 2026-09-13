/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#include "ui/ui_widget_factory.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "ui/ui_image_loader.h"
#include "ui/widgets/w_widget_util.h"

esp_err_t w_sensor_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_sensor_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_sensor_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_button_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_button_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_button_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_slider_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_slider_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_slider_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_graph_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_graph_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_graph_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_empty_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_empty_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_empty_tile_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_light_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_light_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_light_tile_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_heating_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_heating_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_heating_tile_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_weather_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_weather_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_weather_tile_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_todo_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_todo_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_todo_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_media_player_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_media_player_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_media_player_mark_unavailable(ui_widget_instance_t *instance);
void w_media_player_set_visible(ui_widget_instance_t *instance, bool visible);

esp_err_t w_roborock_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_roborock_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_roborock_mark_unavailable(ui_widget_instance_t *instance);
void w_roborock_set_visible(ui_widget_instance_t *instance, bool visible);

esp_err_t w_binary_sensor_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_binary_sensor_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_binary_sensor_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_presence_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_presence_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_presence_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_cover_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_cover_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_cover_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_lock_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_lock_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_lock_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_fan_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_fan_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_fan_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_select_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_select_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_select_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_number_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_number_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_number_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_monitor_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_monitor_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_monitor_tile_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_entity_list_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_entity_list_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_entity_list_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_sensor_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_sensor_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_sensor_tile_mark_unavailable(ui_widget_instance_t *instance);

esp_err_t w_temp_tile_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance);
void w_temp_tile_apply_state(ui_widget_instance_t *instance, const ha_state_t *state);
void w_temp_tile_mark_unavailable(ui_widget_instance_t *instance);

static void ui_widget_factory_recolor_labels(lv_obj_t *obj, lv_color_t color)
{
    if (obj == NULL) {
        return;
    }
    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (child == NULL) {
            continue;
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_style_text_color(child, color, LV_PART_MAIN);
        }
        ui_widget_factory_recolor_labels(child, color);
    }
}

/* Apply per-widget style overrides (tile background colour and label text
 * colour) to the freshly created card. Dynamic labels may still override the
 * text colour in their own apply_state. */
static void ui_widget_factory_apply_style_overrides(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }

    lv_color_t color;
    if (w_parse_hex_color(instance->card_bg_color, &color)) {
        lv_obj_set_style_bg_color(instance->obj, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(instance->obj, LV_OPA_COVER, LV_PART_MAIN);
    }
    if (w_parse_hex_color(instance->label_color, &color)) {
        ui_widget_factory_recolor_labels(instance->obj, color);
    }
}

/* Decode a tile background image from the SD card (or any VFS path) and apply
 * it to the card. The decoded buffer is owned by the instance and released in
 * the LV_EVENT_DELETE callback below. */
static void ui_widget_factory_apply_bg_image(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL || instance->bg_image[0] == '\0') {
        return;
    }

    lv_coord_t w = lv_obj_get_width(instance->obj);
    lv_coord_t h = lv_obj_get_height(instance->obj);
    if (w < 2 || h < 2) {
        return;
    }

    if (!ui_image_load_png_file(instance->bg_image, w, h, &instance->bg_image_dsc)) {
        ESP_LOGW("ui_widget_factory", "bg image load failed for widget %s: %s", instance->id,
                 instance->bg_image);
        return;
    }

    lv_obj_set_style_bg_image_src(instance->obj, &instance->bg_image_dsc, LV_PART_MAIN);
    lv_obj_set_style_bg_image_opa(instance->obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_image_tiled(instance->obj, 0, LV_PART_MAIN);
}

static void ui_widget_factory_bg_delete_cb(lv_event_t *event)
{
    if (event == NULL || lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    ui_widget_instance_t *instance = (ui_widget_instance_t *)lv_event_get_user_data(event);
    if (instance == NULL) {
        return;
    }
    if (instance->bg_image_dsc.data != NULL) {
        heap_caps_free((void *)instance->bg_image_dsc.data);
        instance->bg_image_dsc.data = NULL;
        instance->bg_image_dsc.data_size = 0;
    }
}

esp_err_t ui_widget_factory_create(const ui_widget_def_t *def, lv_obj_t *parent, ui_widget_instance_t *out_instance)
{
    if (def == NULL || parent == NULL || out_instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_instance, 0, sizeof(*out_instance));
    snprintf(out_instance->id, sizeof(out_instance->id), "%s", def->id);
    snprintf(out_instance->type, sizeof(out_instance->type), "%s", def->type);
    snprintf(out_instance->title, sizeof(out_instance->title), "%s", def->title);
    snprintf(out_instance->entity_id, sizeof(out_instance->entity_id), "%s", def->entity_id);
    snprintf(out_instance->secondary_entity_id, sizeof(out_instance->secondary_entity_id), "%s", def->secondary_entity_id);
    snprintf(out_instance->slider_direction, sizeof(out_instance->slider_direction), "%s", def->slider_direction);
    snprintf(out_instance->slider_accent_color, sizeof(out_instance->slider_accent_color), "%s", def->slider_accent_color);
    snprintf(out_instance->button_accent_color, sizeof(out_instance->button_accent_color), "%s", def->button_accent_color);
    snprintf(out_instance->button_mode, sizeof(out_instance->button_mode), "%s", def->button_mode);
    snprintf(out_instance->graph_line_color, sizeof(out_instance->graph_line_color), "%s", def->graph_line_color);
    out_instance->graph_point_count = def->graph_point_count;
    out_instance->graph_time_window_min = def->graph_time_window_min;
    snprintf(out_instance->graph_display_mode, sizeof(out_instance->graph_display_mode), "%s", def->graph_display_mode);
    out_instance->graph_bar_bucket_min = def->graph_bar_bucket_min;
    snprintf(out_instance->style_variant, sizeof(out_instance->style_variant), "%s", def->style_variant);
    snprintf(out_instance->arc_opening, sizeof(out_instance->arc_opening), "%s", def->arc_opening);
    out_instance->sensor_min = def->sensor_min;
    out_instance->sensor_max = def->sensor_max;
    snprintf(out_instance->binary_color_on, sizeof(out_instance->binary_color_on), "%s", def->binary_color_on);
    snprintf(out_instance->binary_color_off, sizeof(out_instance->binary_color_off), "%s", def->binary_color_off);
    snprintf(out_instance->binary_text_on, sizeof(out_instance->binary_text_on), "%s", def->binary_text_on);
    snprintf(out_instance->binary_text_off, sizeof(out_instance->binary_text_off), "%s", def->binary_text_off);
    out_instance->binary_show_title = def->binary_show_title;
    snprintf(out_instance->card_bg_color, sizeof(out_instance->card_bg_color), "%s", def->card_bg_color);
    snprintf(out_instance->label_color, sizeof(out_instance->label_color), "%s", def->label_color);
    snprintf(out_instance->bg_image, sizeof(out_instance->bg_image), "%s", def->bg_image);
    snprintf(out_instance->extra_entity_ids, sizeof(out_instance->extra_entity_ids), "%s", def->extra_entity_ids);
    out_instance->ctx = NULL;

    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (strcmp(def->type, "sensor") == 0) {
        err = w_sensor_create(def, parent, out_instance);
    } else if (strcmp(def->type, "button") == 0) {
        err = w_button_create(def, parent, out_instance);
    } else if (strcmp(def->type, "slider") == 0) {
        err = w_slider_create(def, parent, out_instance);
    } else if (strcmp(def->type, "graph") == 0) {
        err = w_graph_create(def, parent, out_instance);
    } else if (strcmp(def->type, "empty_tile") == 0) {
        err = w_empty_tile_create(def, parent, out_instance);
    } else if (strcmp(def->type, "light_tile") == 0) {
        err = w_light_tile_create(def, parent, out_instance);
    } else if (strcmp(def->type, "heating_tile") == 0) {
        err = w_heating_tile_create(def, parent, out_instance);
    } else if (strcmp(def->type, "weather_tile") == 0 || strcmp(def->type, "weather_3day") == 0) {
        err = w_weather_tile_create(def, parent, out_instance);
    } else if (strcmp(def->type, "todo_list") == 0) {
        err = w_todo_create(def, parent, out_instance);
    } else if (strcmp(def->type, "media_player") == 0) {
        err = w_media_player_create(def, parent, out_instance);
    } else if (strcmp(def->type, "roborock_tile") == 0) {
        err = w_roborock_create(def, parent, out_instance);
    } else if (strcmp(def->type, "binary_sensor") == 0) {
        err = w_binary_sensor_create(def, parent, out_instance);
    } else if (strcmp(def->type, "presence") == 0) {
        err = w_presence_create(def, parent, out_instance);
    } else if (strcmp(def->type, "cover") == 0) {
        err = w_cover_create(def, parent, out_instance);
    } else if (strcmp(def->type, "lock") == 0) {
        err = w_lock_create(def, parent, out_instance);
    } else if (strcmp(def->type, "fan") == 0) {
        err = w_fan_create(def, parent, out_instance);
    } else if (strcmp(def->type, "select") == 0) {
        err = w_select_create(def, parent, out_instance);
    } else if (strcmp(def->type, "number") == 0) {
        err = w_number_create(def, parent, out_instance);
    } else if (strcmp(def->type, "monitor_tile") == 0) {
        err = w_monitor_tile_create(def, parent, out_instance);
    } else if (strcmp(def->type, "entity_list") == 0) {
        err = w_entity_list_create(def, parent, out_instance);
    } else if (strcmp(def->type, "sensor_tile") == 0) {
        err = w_sensor_tile_create(def, parent, out_instance);
    } else if (strcmp(def->type, "temp_tile") == 0) {
        err = w_temp_tile_create(def, parent, out_instance);
    }

    if (err == ESP_OK) {
        ui_widget_factory_apply_style_overrides(out_instance);
        ui_widget_factory_apply_bg_image(out_instance);
        lv_obj_add_event_cb(out_instance->obj, ui_widget_factory_bg_delete_cb, LV_EVENT_DELETE, out_instance);
    }
    return err;
}

void ui_widget_factory_apply_state(ui_widget_instance_t *instance, const ha_state_t *state)
{
    if (instance == NULL || instance->obj == NULL || state == NULL) {
        return;
    }
    if (strcmp(instance->type, "sensor") == 0) {
        w_sensor_apply_state(instance, state);
    } else if (strcmp(instance->type, "button") == 0) {
        w_button_apply_state(instance, state);
    } else if (strcmp(instance->type, "slider") == 0) {
        w_slider_apply_state(instance, state);
    } else if (strcmp(instance->type, "graph") == 0) {
        w_graph_apply_state(instance, state);
    } else if (strcmp(instance->type, "empty_tile") == 0) {
        w_empty_tile_apply_state(instance, state);
    } else if (strcmp(instance->type, "light_tile") == 0) {
        w_light_tile_apply_state(instance, state);
    } else if (strcmp(instance->type, "heating_tile") == 0) {
        w_heating_tile_apply_state(instance, state);
    } else if (strcmp(instance->type, "weather_tile") == 0 || strcmp(instance->type, "weather_3day") == 0) {
        w_weather_tile_apply_state(instance, state);
    } else if (strcmp(instance->type, "todo_list") == 0) {
        w_todo_apply_state(instance, state);
    } else if (strcmp(instance->type, "media_player") == 0) {
        w_media_player_apply_state(instance, state);
    } else if (strcmp(instance->type, "roborock_tile") == 0) {
        w_roborock_apply_state(instance, state);
    } else if (strcmp(instance->type, "binary_sensor") == 0) {
        w_binary_sensor_apply_state(instance, state);
    } else if (strcmp(instance->type, "presence") == 0) {
        w_presence_apply_state(instance, state);
    } else if (strcmp(instance->type, "cover") == 0) {
        w_cover_apply_state(instance, state);
    } else if (strcmp(instance->type, "lock") == 0) {
        w_lock_apply_state(instance, state);
    } else if (strcmp(instance->type, "fan") == 0) {
        w_fan_apply_state(instance, state);
    } else if (strcmp(instance->type, "select") == 0) {
        w_select_apply_state(instance, state);
    } else if (strcmp(instance->type, "number") == 0) {
        w_number_apply_state(instance, state);
    } else if (strcmp(instance->type, "monitor_tile") == 0) {
        w_monitor_tile_apply_state(instance, state);
    } else if (strcmp(instance->type, "entity_list") == 0) {
        w_entity_list_apply_state(instance, state);
    } else if (strcmp(instance->type, "sensor_tile") == 0) {
        w_sensor_tile_apply_state(instance, state);
    } else if (strcmp(instance->type, "temp_tile") == 0) {
        w_temp_tile_apply_state(instance, state);
    }
}

void ui_widget_factory_mark_unavailable(ui_widget_instance_t *instance)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }
    if (strcmp(instance->type, "sensor") == 0) {
        w_sensor_mark_unavailable(instance);
    } else if (strcmp(instance->type, "button") == 0) {
        w_button_mark_unavailable(instance);
    } else if (strcmp(instance->type, "slider") == 0) {
        w_slider_mark_unavailable(instance);
    } else if (strcmp(instance->type, "graph") == 0) {
        w_graph_mark_unavailable(instance);
    } else if (strcmp(instance->type, "empty_tile") == 0) {
        w_empty_tile_mark_unavailable(instance);
    } else if (strcmp(instance->type, "light_tile") == 0) {
        w_light_tile_mark_unavailable(instance);
    } else if (strcmp(instance->type, "heating_tile") == 0) {
        w_heating_tile_mark_unavailable(instance);
    } else if (strcmp(instance->type, "weather_tile") == 0 || strcmp(instance->type, "weather_3day") == 0) {
        w_weather_tile_mark_unavailable(instance);
    } else if (strcmp(instance->type, "todo_list") == 0) {
        w_todo_mark_unavailable(instance);
    } else if (strcmp(instance->type, "media_player") == 0) {
        w_media_player_mark_unavailable(instance);
    } else if (strcmp(instance->type, "roborock_tile") == 0) {
        w_roborock_mark_unavailable(instance);
    } else if (strcmp(instance->type, "binary_sensor") == 0) {
        w_binary_sensor_mark_unavailable(instance);
    } else if (strcmp(instance->type, "presence") == 0) {
        w_presence_mark_unavailable(instance);
    } else if (strcmp(instance->type, "cover") == 0) {
        w_cover_mark_unavailable(instance);
    } else if (strcmp(instance->type, "lock") == 0) {
        w_lock_mark_unavailable(instance);
    } else if (strcmp(instance->type, "fan") == 0) {
        w_fan_mark_unavailable(instance);
    } else if (strcmp(instance->type, "select") == 0) {
        w_select_mark_unavailable(instance);
    } else if (strcmp(instance->type, "number") == 0) {
        w_number_mark_unavailable(instance);
    } else if (strcmp(instance->type, "monitor_tile") == 0) {
        w_monitor_tile_mark_unavailable(instance);
    } else if (strcmp(instance->type, "entity_list") == 0) {
        w_entity_list_mark_unavailable(instance);
    } else if (strcmp(instance->type, "sensor_tile") == 0) {
        w_sensor_tile_mark_unavailable(instance);
    } else if (strcmp(instance->type, "temp_tile") == 0) {
        w_temp_tile_mark_unavailable(instance);
    }
}

void ui_widget_factory_set_visible(ui_widget_instance_t *instance, bool visible)
{
    if (instance == NULL || instance->obj == NULL) {
        return;
    }
    if (instance->visible == visible) {
        return;
    }
    instance->visible = visible;

    if (strcmp(instance->type, "media_player") == 0) {
        w_media_player_set_visible(instance, visible);
    } else if (strcmp(instance->type, "roborock_tile") == 0) {
        w_roborock_set_visible(instance, visible);
    }
}
