/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * HTTP endpoints for the built-in MIPI-CSI camera (Guition JC8012P4A1C).
 */
#include "api/api_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "camera/local_camera.h"
#include "util/log_tags.h"

#define CAMERA_STREAM_FRAME_MS 500
#define CAMERA_STREAM_STACK_WORDS 4096
#define CAMERA_STREAM_PRIO 5

static bool s_stream_enabled = false;
static TaskHandle_t s_stream_task = NULL;

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

esp_err_t api_camera_local_snapshot_get_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_len = 0;

    esp_err_t err = local_camera_snapshot_jpeg(&jpeg, &jpeg_len);
    if (err != ESP_OK || jpeg == NULL || jpeg_len == 0) {
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
            httpd_resp_set_status(req, "503 Service Unavailable");
            return httpd_resp_sendstr(req, "{\"error\":\"camera_not_ready\"}");
        }
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"snapshot_failed\"}");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t send_err = httpd_resp_send(req, (const char *)jpeg, (ssize_t)jpeg_len);
    free(jpeg);
    return send_err;
}

esp_err_t api_camera_local_status_get_handler(httpd_req_t *req)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"running\":%s,\"width\":%d,\"height\":%d,"
             "\"motion_wake\":%s,\"motion_threshold\":%u,\"jpeg_quality\":%u,"
             "\"hflip\":%s,\"vflip\":%s}",
             local_camera_is_running() ? "true" : "false",
             local_camera_width(),
             local_camera_height(),
             local_camera_get_motion_wake() ? "true" : "false",
             (unsigned)local_camera_get_motion_threshold(),
             (unsigned)local_camera_get_jpeg_quality(),
             local_camera_get_hflip() ? "true" : "false",
             local_camera_get_vflip() ? "true" : "false");

    set_json_headers(req);
    return httpd_resp_sendstr(req, json);
}

void api_camera_local_set_stream_enabled(bool enabled)
{
    s_stream_enabled = enabled;
}

bool api_camera_local_get_stream_enabled(void)
{
    return s_stream_enabled;
}

static void camera_stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    char header[128];

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");

    while (s_stream_enabled && local_camera_is_running()) {
        uint8_t *jpeg = NULL;
        size_t jpeg_len = 0;

        esp_err_t err = local_camera_snapshot_jpeg(&jpeg, &jpeg_len);
        if (err != ESP_OK || jpeg == NULL || jpeg_len == 0) {
            free(jpeg);
            vTaskDelay(pdMS_TO_TICKS(CAMERA_STREAM_FRAME_MS));
            continue;
        }

        int hdr_len = snprintf(header, sizeof(header),
                               "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                               (unsigned)jpeg_len);
        if (hdr_len <= 0 || (size_t)hdr_len >= sizeof(header)) {
            free(jpeg);
            break;
        }

        esp_err_t send_err = httpd_resp_send_chunk(req, header, (ssize_t)hdr_len);
        if (send_err == ESP_OK) {
            send_err = httpd_resp_send_chunk(req, (const char *)jpeg, (ssize_t)jpeg_len);
        }
        if (send_err == ESP_OK) {
            send_err = httpd_resp_send_chunk(req, "\r\n", 2);
        }
        free(jpeg);
        if (send_err != ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(CAMERA_STREAM_FRAME_MS));
    }

    httpd_req_async_handler_complete(req);
    s_stream_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t api_camera_local_stream_get_handler(httpd_req_t *req)
{
    if (!s_stream_enabled) {
        set_json_headers(req);
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"stream_disabled\"}");
    }
    if (s_stream_task != NULL) {
        set_json_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"stream_busy\"}");
    }
    if (!local_camera_is_running()) {
        set_json_headers(req);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"camera_not_ready\"}");
    }

    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK || copy == NULL) {
        set_json_headers(req);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"stream_start_failed\"}");
    }

    if (xTaskCreate(camera_stream_task, "cam_stream", CAMERA_STREAM_STACK_WORDS,
                    copy, CAMERA_STREAM_PRIO, &s_stream_task) != pdPASS) {
        httpd_req_async_handler_complete(copy);
        s_stream_task = NULL;
    }

    /* The socket is owned by the async task from this point on. */
    return ESP_OK;
}
