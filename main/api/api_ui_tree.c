/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Diagnostic endpoint: dumps the live LVGL object tree (class, geometry,
 * z-order, touch-relevant flags) so touch dead-band / overlap issues can be
 * diagnosed from the web instead of by guesswork.
 */
#include "api/api_routes.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"

#include "cJSON.h"
#include "lvgl.h"

#include "drivers/display_init.h"
#include "util/log_tags.h"

#define UI_TREE_LOCK_TIMEOUT_MS 1500U

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static const char *class_name(const lv_obj_t *obj)
{
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    if (cls == &lv_slider_class) {
        return "slider";
    }
    if (cls == &lv_label_class) {
        return "label";
    }
    if (cls == &lv_button_class) {
        return "button";
    }
    if (cls == &lv_image_class) {
        return "image";
    }
    if (cls == &lv_chart_class) {
        return "chart";
    }
    if (cls == &lv_arc_class) {
        return "arc";
    }
    if (cls == &lv_switch_class) {
        return "switch";
    }
    if (cls == &lv_led_class) {
        return "led";
    }
    if (cls == &lv_line_class) {
        return "line";
    }
    if (cls == &lv_roller_class) {
        return "roller";
    }
    if (cls == &lv_dropdown_class) {
        return "dropdown";
    }
    if (cls == &lv_textarea_class) {
        return "textarea";
    }
    if (cls == &lv_keyboard_class) {
        return "keyboard";
    }
    if (cls == &lv_bar_class) {
        return "bar";
    }
    if (cls == &lv_spinner_class) {
        return "spinner";
    }
    if (cls == &lv_canvas_class) {
        return "canvas";
    }
    if (cls == &lv_scale_class) {
        return "scale";
    }
    if (cls == &lv_obj_class) {
        return "obj";
    }
    return "widget";
}

static void dump_obj(cJSON *parent_array, const lv_obj_t *obj)
{
    uint32_t child_cnt = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < child_cnt; i++) {
        const lv_obj_t *child = lv_obj_get_child(obj, i);
        if (child == NULL) {
            continue;
        }

        cJSON *node = cJSON_CreateObject();
        if (node == NULL) {
            return;
        }
        cJSON_AddItemToArray(parent_array, node);

        cJSON_AddStringToObject(node, "class", class_name(child));
        cJSON_AddNumberToObject(node, "x", lv_obj_get_x(child));
        cJSON_AddNumberToObject(node, "y", lv_obj_get_y(child));
        cJSON_AddNumberToObject(node, "w", lv_obj_get_width(child));
        cJSON_AddNumberToObject(node, "h", lv_obj_get_height(child));
        cJSON_AddNumberToObject(node, "z", (double)i);
        cJSON_AddBoolToObject(node, "hidden", lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN));
        cJSON_AddBoolToObject(node, "clickable", lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE));
        cJSON_AddBoolToObject(node, "scrollable", lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE));
        cJSON_AddBoolToObject(node, "event_bubble", lv_obj_has_flag(child, LV_OBJ_FLAG_EVENT_BUBBLE));

        cJSON *children = cJSON_CreateArray();
        if (children != NULL) {
            cJSON_AddItemToObject(node, "children", children);
            dump_obj(children, child);
        }
    }
}

esp_err_t api_ui_tree_get_handler(httpd_req_t *req)
{
    if (!display_lock(UI_TREE_LOCK_TIMEOUT_MS)) {
        set_json_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"display busy\"}");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        display_unlock();
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);

    lv_obj_t *screen = lv_screen_active();
    if (screen != NULL) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "screen", arr);
        dump_obj(arr, screen);
    }

    lv_obj_t *top = lv_layer_top();
    if (top != NULL) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "layer_top", arr);
        dump_obj(arr, top);
    }

    lv_obj_t *sys = lv_layer_sys();
    if (sys != NULL) {
        cJSON *arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "layer_sys", arr);
        dump_obj(arr, sys);
    }

    display_unlock();

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}
