/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * SD-card backup / restore API (JC8012P4A1C 10.1" panel only):
 *   POST /api/backup  -> serialize config (settings, layout, cameras, active
 *                        theme, display policy) into /sdcard/backup/panel-backup.json
 *   POST /api/restore -> read the backup file and write everything back.
 *
 * The backup file intentionally contains the FULL settings including WiFi
 * password and HA access token: it is written to the user's own SD card so a
 * panel can be restored 1:1 without re-entering credentials.
 */
#include "api/api_routes.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "drivers/display_init.h"
#include "layout/layout_store.h"
#include "settings/runtime_settings.h"
#include "util/log_tags.h"

#define TAG TAG_SD

#define BACKUP_DIR "/sdcard/backup"
#define BACKUP_PATH "/sdcard/backup/panel-backup.json"
#define BACKUP_MAX_FILE_BYTES (64U * 1024U)
#define BACKUP_FORMAT_VERSION 1

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", (message != NULL) ? message : "Invalid request");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static esp_err_t backup_send_json(httpd_req_t *req, cJSON *root)
{
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
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

/* ------------------------------------------------------------------ */
/* File helpers                                                        */
/* ------------------------------------------------------------------ */
static esp_err_t backup_read_text(const char *path, size_t max_len, char **out_text)
{
    if (path == NULL || out_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > max_len) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    char *buf = calloc((size_t)size + 1U, sizeof(char));
    if (buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read = fread(buf, 1U, (size_t)size, f);
    fclose(f);
    if (read != (size_t)size) {
        free(buf);
        return ESP_FAIL;
    }
    *out_text = buf;
    return ESP_OK;
}

static esp_err_t backup_write_text(const char *path, const char *text)
{
    if (path == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return ESP_FAIL;
    }
    size_t len = strlen(text);
    size_t written = fwrite(text, 1U, len, f);
    fclose(f);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

static esp_err_t backup_ensure_dir(void)
{
    (void)mkdir("/sdcard", 0777);
    int rc = mkdir(BACKUP_DIR, 0777);
    if (rc == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/* Settings <-> JSON (full struct, including secrets)                  */
/* ------------------------------------------------------------------ */
static void backup_json_copy_string(cJSON *obj, const char *key, const char *dst)
{
    if (obj != NULL && key != NULL && dst != NULL) {
        cJSON_AddStringToObject(obj, key, dst);
    }
}

static void backup_json_get_string(cJSON *obj, const char *key, char *dst, size_t dst_len)
{
    if (obj == NULL || key == NULL || dst == NULL || dst_len == 0) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dst, item->valuestring, dst_len);
    }
}

static void backup_json_get_int(cJSON *obj, const char *key, int *dst, int min, int max)
{
    if (obj == NULL || key == NULL || dst == NULL) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item)) {
        int value = (int)item->valuedouble;
        if (value < min) {
            value = min;
        }
        if (value > max) {
            value = max;
        }
        *dst = value;
    }
}

static void backup_json_get_bool(cJSON *obj, const char *key, bool *dst)
{
    if (obj == NULL || key == NULL || dst == NULL) {
        return;
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(item)) {
        *dst = cJSON_IsTrue(item);
    }
}

static cJSON *backup_settings_to_json(const runtime_settings_t *s)
{
    if (s == NULL) {
        return NULL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_CreateObject();
    cJSON *ha = cJSON_CreateObject();
    cJSON *time_cfg = cJSON_CreateObject();
    cJSON *ui = cJSON_CreateObject();
    cJSON *xiaozhi = cJSON_CreateObject();
    cJSON *sd = cJSON_CreateObject();
    cJSON *network = cJSON_CreateObject();
    cJSON *camera = cJSON_CreateObject();
    cJSON *system = cJSON_CreateObject();
    cJSON *audio = cJSON_CreateObject();
    if (root == NULL || wifi == NULL || ha == NULL || time_cfg == NULL || ui == NULL || xiaozhi == NULL ||
        sd == NULL || network == NULL || camera == NULL || system == NULL || audio == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(wifi);
        cJSON_Delete(ha);
        cJSON_Delete(time_cfg);
        cJSON_Delete(ui);
        cJSON_Delete(xiaozhi);
        cJSON_Delete(sd);
        cJSON_Delete(network);
        cJSON_Delete(camera);
        cJSON_Delete(system);
        cJSON_Delete(audio);
        return NULL;
    }

    backup_json_copy_string(wifi, "ssid", s->wifi_ssid);
    backup_json_copy_string(wifi, "password", s->wifi_password);
    backup_json_copy_string(wifi, "country_code", s->wifi_country_code);
    backup_json_copy_string(wifi, "bssid", s->wifi_bssid);
    cJSON_AddItemToObject(root, "wifi", wifi);

    backup_json_copy_string(ha, "ws_url", s->ha_ws_url);
    backup_json_copy_string(ha, "access_token", s->ha_access_token);
    cJSON_AddBoolToObject(ha, "rest_enabled", s->ha_rest_enabled);
    cJSON_AddItemToObject(root, "ha", ha);

    backup_json_copy_string(time_cfg, "ntp_server", s->ntp_server);
    backup_json_copy_string(time_cfg, "timezone", s->time_tz);
    cJSON_AddItemToObject(root, "time", time_cfg);

    backup_json_copy_string(ui, "language", s->ui_language);
    cJSON_AddItemToObject(root, "ui", ui);

    backup_json_copy_string(xiaozhi, "server", s->xiaozhi_server);
    backup_json_copy_string(xiaozhi, "device", s->xiaozhi_device);
    backup_json_copy_string(xiaozhi, "ota_url", s->xiaozhi_ota_url);
    cJSON_AddBoolToObject(xiaozhi, "enabled", s->xiaozhi_enabled);
    backup_json_copy_string(xiaozhi, "token", s->xiaozhi_token);
    cJSON_AddItemToObject(root, "xiaozhi", xiaozhi);

    cJSON_AddBoolToObject(sd, "logging_enabled", s->sd_logging_enabled);
    cJSON_AddNumberToObject(sd, "flush_interval_s", s->sd_flush_interval_s);
    cJSON_AddBoolToObject(sd, "log_system_enabled", s->sd_log_system_enabled);
    cJSON_AddBoolToObject(sd, "log_sensors_enabled", s->sd_log_sensors_enabled);
    cJSON_AddBoolToObject(sd, "log_camera_enabled", s->sd_log_camera_enabled);
    cJSON_AddItemToObject(root, "sd", sd);

    cJSON_AddBoolToObject(network, "static_ip_enabled", s->wifi_static_ip_enabled);
    backup_json_copy_string(network, "static_ip", s->wifi_static_ip);
    backup_json_copy_string(network, "static_netmask", s->wifi_static_netmask);
    backup_json_copy_string(network, "static_gateway", s->wifi_static_gateway);
    backup_json_copy_string(network, "static_dns", s->wifi_static_dns);
    cJSON_AddItemToObject(root, "network", network);

    cJSON_AddBoolToObject(camera, "enabled", s->camera_enabled);
    cJSON_AddBoolToObject(camera, "motion_wake", s->camera_motion_wake);
    cJSON_AddNumberToObject(camera, "motion_threshold", s->camera_motion_threshold);
    cJSON_AddNumberToObject(camera, "jpeg_quality", s->camera_jpeg_quality);
    cJSON_AddBoolToObject(camera, "hflip", s->camera_hflip);
    cJSON_AddBoolToObject(camera, "vflip", s->camera_vflip);
    cJSON_AddBoolToObject(camera, "stream_enabled", s->camera_stream_enabled);
    cJSON_AddItemToObject(root, "camera", camera);

    cJSON_AddNumberToObject(system, "daily_restart_hour", s->daily_restart_hour);
    cJSON_AddBoolToObject(system, "touch_test", s->touch_test);
    cJSON_AddItemToObject(root, "system", system);

    cJSON_AddNumberToObject(audio, "volume", s->audio_volume);
    cJSON_AddItemToObject(root, "audio", audio);

    return root;
}

static void backup_settings_from_json(cJSON *root, runtime_settings_t *s)
{
    if (root == NULL || s == NULL) {
        return;
    }

    cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        backup_json_get_string(wifi, "ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
        backup_json_get_string(wifi, "password", s->wifi_password, sizeof(s->wifi_password));
        backup_json_get_string(wifi, "country_code", s->wifi_country_code, sizeof(s->wifi_country_code));
        backup_json_get_string(wifi, "bssid", s->wifi_bssid, sizeof(s->wifi_bssid));
    }

    cJSON *ha = cJSON_GetObjectItemCaseSensitive(root, "ha");
    if (cJSON_IsObject(ha)) {
        backup_json_get_string(ha, "ws_url", s->ha_ws_url, sizeof(s->ha_ws_url));
        backup_json_get_string(ha, "access_token", s->ha_access_token, sizeof(s->ha_access_token));
        backup_json_get_bool(ha, "rest_enabled", &s->ha_rest_enabled);
    }

    cJSON *time_cfg = cJSON_GetObjectItemCaseSensitive(root, "time");
    if (cJSON_IsObject(time_cfg)) {
        backup_json_get_string(time_cfg, "ntp_server", s->ntp_server, sizeof(s->ntp_server));
        backup_json_get_string(time_cfg, "timezone", s->time_tz, sizeof(s->time_tz));
    }

    cJSON *ui = cJSON_GetObjectItemCaseSensitive(root, "ui");
    if (cJSON_IsObject(ui)) {
        backup_json_get_string(ui, "language", s->ui_language, sizeof(s->ui_language));
    }

    cJSON *xiaozhi = cJSON_GetObjectItemCaseSensitive(root, "xiaozhi");
    if (cJSON_IsObject(xiaozhi)) {
        backup_json_get_string(xiaozhi, "server", s->xiaozhi_server, sizeof(s->xiaozhi_server));
        backup_json_get_string(xiaozhi, "device", s->xiaozhi_device, sizeof(s->xiaozhi_device));
        backup_json_get_string(xiaozhi, "ota_url", s->xiaozhi_ota_url, sizeof(s->xiaozhi_ota_url));
        backup_json_get_bool(xiaozhi, "enabled", &s->xiaozhi_enabled);
        backup_json_get_string(xiaozhi, "token", s->xiaozhi_token, sizeof(s->xiaozhi_token));
    }

    cJSON *sd = cJSON_GetObjectItemCaseSensitive(root, "sd");
    if (cJSON_IsObject(sd)) {
        backup_json_get_bool(sd, "logging_enabled", &s->sd_logging_enabled);
        backup_json_get_int(sd, "flush_interval_s", &s->sd_flush_interval_s, 5, 300);
        backup_json_get_bool(sd, "log_system_enabled", &s->sd_log_system_enabled);
        backup_json_get_bool(sd, "log_sensors_enabled", &s->sd_log_sensors_enabled);
        backup_json_get_bool(sd, "log_camera_enabled", &s->sd_log_camera_enabled);
    }

    cJSON *network = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (cJSON_IsObject(network)) {
        backup_json_get_bool(network, "static_ip_enabled", &s->wifi_static_ip_enabled);
        backup_json_get_string(network, "static_ip", s->wifi_static_ip, sizeof(s->wifi_static_ip));
        backup_json_get_string(network, "static_netmask", s->wifi_static_netmask, sizeof(s->wifi_static_netmask));
        backup_json_get_string(network, "static_gateway", s->wifi_static_gateway, sizeof(s->wifi_static_gateway));
        backup_json_get_string(network, "static_dns", s->wifi_static_dns, sizeof(s->wifi_static_dns));
    }

    cJSON *camera = cJSON_GetObjectItemCaseSensitive(root, "camera");
    if (cJSON_IsObject(camera)) {
        backup_json_get_bool(camera, "enabled", &s->camera_enabled);
        backup_json_get_bool(camera, "motion_wake", &s->camera_motion_wake);
        backup_json_get_int(camera, "motion_threshold", &s->camera_motion_threshold, 1, 64);
        backup_json_get_int(camera, "jpeg_quality", &s->camera_jpeg_quality, 10, 95);
        backup_json_get_bool(camera, "hflip", &s->camera_hflip);
        backup_json_get_bool(camera, "vflip", &s->camera_vflip);
        backup_json_get_bool(camera, "stream_enabled", &s->camera_stream_enabled);
    }

    cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
    if (cJSON_IsObject(system)) {
        backup_json_get_int(system, "daily_restart_hour", &s->daily_restart_hour, -1, 23);
        backup_json_get_bool(system, "touch_test", &s->touch_test);
    }

    cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio");
    if (cJSON_IsObject(audio)) {
        backup_json_get_int(audio, "volume", &s->audio_volume, 0, 100);
    }
}

/* ------------------------------------------------------------------ */
/* Display power policy <-> JSON                                       */
/* ------------------------------------------------------------------ */
static cJSON *backup_display_to_json(const display_power_config_t *c)
{
    if (c == NULL) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddNumberToObject(o, "active_brightness_percent", c->active_brightness_percent);
    cJSON_AddNumberToObject(o, "dim_brightness_percent", c->dim_brightness_percent);
    cJSON_AddNumberToObject(o, "dim_timeout_ms", (double)c->dim_timeout_ms);
    cJSON_AddNumberToObject(o, "off_timeout_ms", (double)c->off_timeout_ms);
    cJSON_AddBoolToObject(o, "night_mode_enabled", c->night_mode_enabled);
    cJSON_AddNumberToObject(o, "night_start_hour", c->night_start_hour);
    cJSON_AddNumberToObject(o, "night_end_hour", c->night_end_hour);
    cJSON_AddBoolToObject(o, "screensaver_enabled", c->screensaver_enabled);
    cJSON_AddNumberToObject(o, "screensaver_brightness_percent", c->screensaver_brightness_percent);
    cJSON_AddBoolToObject(o, "screensaver_clock_enabled", c->screensaver_clock_enabled);
    return o;
}

static void backup_display_from_json(cJSON *o, display_power_config_t *c)
{
    if (o == NULL || c == NULL) {
        return;
    }
    backup_json_get_int(o, "active_brightness_percent", &c->active_brightness_percent, 0, 100);
    backup_json_get_int(o, "dim_brightness_percent", &c->dim_brightness_percent, 0, 100);
    backup_json_get_int(o, "night_start_hour", &c->night_start_hour, 0, 23);
    backup_json_get_int(o, "night_end_hour", &c->night_end_hour, 0, 23);
    backup_json_get_int(o, "screensaver_brightness_percent", &c->screensaver_brightness_percent, 0, 100);

    cJSON *dim_ms = cJSON_GetObjectItemCaseSensitive(o, "dim_timeout_ms");
    if (cJSON_IsNumber(dim_ms)) {
        c->dim_timeout_ms = (uint32_t)(dim_ms->valuedouble < 0 ? 0 : dim_ms->valuedouble);
    }
    cJSON *off_ms = cJSON_GetObjectItemCaseSensitive(o, "off_timeout_ms");
    if (cJSON_IsNumber(off_ms)) {
        c->off_timeout_ms = (uint32_t)(off_ms->valuedouble < 0 ? 0 : off_ms->valuedouble);
    }

    backup_json_get_bool(o, "night_mode_enabled", &c->night_mode_enabled);
    backup_json_get_bool(o, "screensaver_enabled", &c->screensaver_enabled);
    backup_json_get_bool(o, "screensaver_clock_enabled", &c->screensaver_clock_enabled);
}

/* ------------------------------------------------------------------ */
/* Restart scheduling (mirrors the settings PUT reboot path)           */
/* ------------------------------------------------------------------ */
static esp_timer_handle_t s_backup_restart_timer = NULL;

static void backup_restart_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void backup_schedule_restart(void)
{
    if (s_backup_restart_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = &backup_restart_timer_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "backup_restart",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&timer_args, &s_backup_restart_timer) != ESP_OK) {
            esp_restart();
            return;
        }
    }
    if (esp_timer_is_active(s_backup_restart_timer)) {
        (void)esp_timer_stop(s_backup_restart_timer);
    }
    if (esp_timer_start_once(s_backup_restart_timer, 1500ULL * 1000ULL) != ESP_OK) {
        esp_restart();
    }
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */
esp_err_t api_backup_post_handler(httpd_req_t *req)
{
    (void)req;

    if (backup_ensure_dir() != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Cannot create /sdcard/backup");
    }

    runtime_settings_t settings;
    runtime_settings_set_defaults(&settings);
    esp_err_t settings_err = runtime_settings_load(&settings);
    if (settings_err != ESP_OK) {
        ESP_LOGW(TAG, "Backup: settings load failed (%s), using defaults", esp_err_to_name(settings_err));
    }

    char *layout = NULL;
    esp_err_t layout_err = layout_store_load(&layout);

    char *cameras = NULL;
    (void)backup_read_text(APP_CAMERAS_PATH, 16384, &cameras);

    char *theme_active = NULL;
    (void)backup_read_text(APP_THEME_ACTIVE_PATH, 256, &theme_active);

    cJSON *settings_json = backup_settings_to_json(&settings);
    cJSON *display_json = NULL;
    display_power_config_t display_cfg;
    display_get_power_config(&display_cfg);
    display_json = backup_display_to_json(&display_cfg);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL || settings_json == NULL || display_json == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(settings_json);
        cJSON_Delete(display_json);
        free(layout);
        free(cameras);
        free(theme_active);
        return httpd_resp_send_500(req);
    }

    cJSON_AddStringToObject(root, "type", "betta-ha-panel-backup");
    cJSON_AddNumberToObject(root, "version", BACKUP_FORMAT_VERSION);
    cJSON_AddNumberToObject(root, "created_utc", (double)time(NULL));
    cJSON_AddItemToObject(root, "settings", settings_json);

    if (layout_err == ESP_OK && layout != NULL) {
        cJSON_AddStringToObject(root, "layout", layout);
    } else {
        cJSON_AddNullToObject(root, "layout");
    }

    if (cameras != NULL) {
        cJSON_AddStringToObject(root, "cameras", cameras);
    } else {
        cJSON_AddNullToObject(root, "cameras");
    }

    if (theme_active != NULL) {
        cJSON_AddStringToObject(root, "theme_active", theme_active);
    } else {
        cJSON_AddNullToObject(root, "theme_active");
    }

    cJSON_AddItemToObject(root, "display", display_json);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(layout);
    free(cameras);
    free(theme_active);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    esp_err_t write_err = backup_write_text(BACKUP_PATH, payload);
    cJSON_free(payload);
    if (write_err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Failed to write backup file");
    }

    ESP_LOGI(TAG, "Config backup written to %s", BACKUP_PATH);

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "path", BACKUP_PATH);
    cJSON_AddBoolToObject(resp, "layout_backed_up", layout_err == ESP_OK);
    return backup_send_json(req, resp);
}

esp_err_t api_restore_post_handler(httpd_req_t *req)
{
    /* Require an explicit {"confirm":true} so a stray request can never
     * overwrite the live panel configuration. */
    if (req->content_len < 0 || req->content_len > 256) {
        return send_json_error(req, "400 Bad Request", "Invalid payload size");
    }

    bool confirmed = false;
    bool reboot = true;

    if (req->content_len > 0) {
        char *buf = calloc((size_t)req->content_len + 1U, sizeof(char));
        if (buf == NULL) {
            return httpd_resp_send_500(req);
        }
        int received = 0;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, buf + received, req->content_len - received);
            if (r <= 0) {
                free(buf);
                return send_json_error(req, "400 Bad Request", "Failed to read request body");
            }
            received += r;
        }

        cJSON *body = cJSON_Parse(buf);
        free(buf);
        if (body == NULL) {
            return send_json_error(req, "400 Bad Request", "Invalid JSON");
        }
        cJSON *confirm = cJSON_GetObjectItemCaseSensitive(body, "confirm");
        confirmed = cJSON_IsTrue(confirm);
        cJSON *reboot_item = cJSON_GetObjectItemCaseSensitive(body, "reboot");
        if (cJSON_IsBool(reboot_item)) {
            reboot = cJSON_IsTrue(reboot_item);
        }
        cJSON_Delete(body);
    }

    if (!confirmed) {
        return send_json_error(req, "400 Bad Request", "Missing confirm:true");
    }

    char *backup_text = NULL;
    esp_err_t read_err = backup_read_text(BACKUP_PATH, BACKUP_MAX_FILE_BYTES, &backup_text);
    if (read_err != ESP_OK || backup_text == NULL) {
        return send_json_error(req, "404 Not Found", "No backup file on SD card");
    }

    cJSON *root = cJSON_Parse(backup_text);
    free(backup_text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "Backup file is not valid JSON");
    }

    esp_err_t first_err = ESP_OK;
    int restored = 0;

    /* 1) Settings (public file + NVS secrets). */
    cJSON *settings_json = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsObject(settings_json)) {
        runtime_settings_t settings;
        runtime_settings_set_defaults(&settings);
        backup_settings_from_json(settings_json, &settings);
        if (runtime_settings_save(&settings) == ESP_OK) {
            restored++;
        } else if (first_err == ESP_OK) {
            first_err = ESP_FAIL;
        }
    }

    /* 2) Layout. */
    cJSON *layout_item = cJSON_GetObjectItemCaseSensitive(root, "layout");
    if (cJSON_IsString(layout_item) && layout_item->valuestring != NULL) {
        if (layout_store_save(layout_item->valuestring) == ESP_OK) {
            restored++;
        } else if (first_err == ESP_OK) {
            first_err = ESP_FAIL;
        }
    }

    /* 3) Cameras. */
    cJSON *cameras_item = cJSON_GetObjectItemCaseSensitive(root, "cameras");
    if (cJSON_IsString(cameras_item) && cameras_item->valuestring != NULL) {
        if (backup_write_text(APP_CAMERAS_PATH, cameras_item->valuestring) == ESP_OK) {
            restored++;
        } else if (first_err == ESP_OK) {
            first_err = ESP_FAIL;
        }
    }

    /* 4) Active theme. */
    cJSON *theme_item = cJSON_GetObjectItemCaseSensitive(root, "theme_active");
    if (cJSON_IsString(theme_item) && theme_item->valuestring != NULL && theme_item->valuestring[0] != '\0') {
        if (backup_write_text(APP_THEME_ACTIVE_PATH, theme_item->valuestring) == ESP_OK) {
            restored++;
        } else if (first_err == ESP_OK) {
            first_err = ESP_FAIL;
        }
    }

    /* 5) Display power policy (NVS-backed). */
    cJSON *display_item = cJSON_GetObjectItemCaseSensitive(root, "display");
    if (cJSON_IsObject(display_item)) {
        display_power_config_t display_cfg;
        display_get_power_config(&display_cfg);
        backup_display_from_json(display_item, &display_cfg);
        display_set_power_config(&display_cfg);
        restored++;
    }

    cJSON_Delete(root);

    if (first_err != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "Some config items failed to restore");
    }

    ESP_LOGI(TAG, "Config restored from %s (%d items), reboot=%d", BACKUP_PATH, restored, (int)reboot);

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddNumberToObject(resp, "restored", restored);
    cJSON_AddBoolToObject(resp, "rebooting", reboot);

    /* Deliver the response first, then reboot so the new settings/layout are
     * applied cleanly on the next boot. */
    esp_err_t send_err = backup_send_json(req, resp);
    if (reboot) {
        backup_schedule_restart();
    }
    return send_err;
}
