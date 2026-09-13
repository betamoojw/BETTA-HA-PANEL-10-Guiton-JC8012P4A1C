/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "app_config.h"

typedef struct {
    char wifi_ssid[APP_WIFI_SSID_MAX_LEN];
    char wifi_password[APP_WIFI_PASSWORD_MAX_LEN];
    char wifi_country_code[APP_WIFI_COUNTRY_CODE_MAX_LEN];
    char wifi_bssid[APP_WIFI_BSSID_MAX_LEN];
    char ha_ws_url[APP_HA_WS_URL_MAX_LEN];
    char ha_access_token[APP_HA_ACCESS_TOKEN_MAX_LEN];
    bool ha_rest_enabled;
    char ntp_server[APP_NTP_SERVER_MAX_LEN];
    char time_tz[APP_TIME_TZ_MAX_LEN];
    char ui_language[APP_UI_LANGUAGE_MAX_LEN];
    char xiaozhi_server[APP_XIAOZHI_SERVER_MAX_LEN];
    char xiaozhi_device[APP_XIAOZHI_DEVICE_MAX_LEN];
    char xiaozhi_token[APP_XIAOZHI_TOKEN_MAX_LEN];
    char xiaozhi_ota_url[APP_XIAOZHI_OTA_URL_MAX_LEN];
    bool xiaozhi_enabled;
    bool sd_logging_enabled;

    /* Network: static IPv4 configuration (DHCP when disabled). */
    bool wifi_static_ip_enabled;
    char wifi_static_ip[16];
    char wifi_static_netmask[16];
    char wifi_static_gateway[16];
    char wifi_static_dns[16];

    /* Local camera (JC variant). */
    bool camera_enabled;
    bool camera_motion_wake;
    int camera_motion_threshold; /* 1..64; higher = less sensitive */
    int camera_jpeg_quality;     /* 10..95 */
    bool camera_hflip;
    bool camera_vflip;
    bool camera_stream_enabled; /* MJPEG live stream for HA (http://<ip>/api/camera/stream) */

    /* SD logging. */
    int sd_flush_interval_s; /* 5..300 */
    bool sd_log_system_enabled;
    bool sd_log_sensors_enabled;
    bool sd_log_camera_enabled;

    /* System. */
    int daily_restart_hour; /* -1 = fixed 24h from boot; 0..23 = local hour */
    bool touch_test;        /* Touch debug overlay (red dots) for dead-zone testing */

    /* Audio. */
    int audio_volume; /* 0..100 */
} runtime_settings_t;

void runtime_settings_set_defaults(runtime_settings_t *out);
esp_err_t runtime_settings_init(void);
esp_err_t runtime_settings_load(runtime_settings_t *out);
esp_err_t runtime_settings_save(const runtime_settings_t *settings);
bool runtime_settings_has_wifi(const runtime_settings_t *settings);
bool runtime_settings_has_ha(const runtime_settings_t *settings);
bool runtime_settings_has_xiaozhi(const runtime_settings_t *settings);
