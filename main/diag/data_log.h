/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Resilient SD-card data logging for the JC8012P4A1C 10.1" panel.
 *
 * All file I/O happens on a single low-priority writer task with a bounded
 * queue, so no producer (HA client, UI bindings, syslog capture) can ever be
 * blocked by a slow or dying card.  Producers drop the newest record when the
 * queue is full, never block, and never touch the filesystem themselves.
 *
 * The module is a no-op for non-JC variants (no SD slot); the header provides
 * inline stubs there so call sites in shared code need no #if guards.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#if CONFIG_APP_PANEL_VARIANT_10INCH_JC

esp_err_t data_log_init(void);
void data_log_set_enabled(bool enabled);
void data_log_set_flush_interval_s(int seconds);
void data_log_set_log_system_enabled(bool enabled);
void data_log_set_log_sensors_enabled(bool enabled);
void data_log_set_log_camera_enabled(bool enabled);
bool data_log_is_enabled(void);
bool data_log_is_lost(void);

/* Append a raw line (already "\n"-terminated) to /sdcard/panel/system.log. */
void data_log_mirror_syslog(const char *line, size_t len);

/* Record one sensor state change to /sdcard/panel/sensors.csv. */
void data_log_sensor(const char *entity_id, const char *state);

/* Record one raw touch sample to /sdcard/panel/touch.csv (CSV):
 *   ms, raw_x, raw_y, raw_f, out_x, out_y, out_f
 * Written uncapped so a slow drag through a dead band is captured sample by
 * sample. The producer never blocks (bounded queue, drop-newest). */
void data_log_touch(uint32_t ms,
                    uint16_t raw_x, uint16_t raw_y, uint8_t raw_f,
                    uint16_t out_x, uint16_t out_y, uint8_t out_f);

/* Persist a JPEG blob to /sdcard/panel/camera/. */
void data_log_save_jpeg(const uint8_t *data, size_t len);

/* Suspend/resume the writer task.  Suspend is used by the "format as new"
 * flow so no file handle can be open while the card is unmounted. */
void data_log_suspend(void);
esp_err_t data_log_resume(void);

#else /* !CONFIG_APP_PANEL_VARIANT_10INCH_JC */

static inline esp_err_t data_log_init(void) { return ESP_ERR_NOT_SUPPORTED; }
static inline void data_log_set_enabled(bool enabled) { (void)enabled; }
static inline void data_log_set_flush_interval_s(int seconds) { (void)seconds; }
static inline void data_log_set_log_system_enabled(bool enabled) { (void)enabled; }
static inline void data_log_set_log_sensors_enabled(bool enabled) { (void)enabled; }
static inline void data_log_set_log_camera_enabled(bool enabled) { (void)enabled; }
static inline bool data_log_is_enabled(void) { return false; }
static inline bool data_log_is_lost(void) { return false; }
static inline void data_log_mirror_syslog(const char *line, size_t len)
{
    (void)line;
    (void)len;
}
static inline void data_log_sensor(const char *entity_id, const char *state)
{
    (void)entity_id;
    (void)state;
}
static inline void data_log_touch(uint32_t ms,
                                  uint16_t raw_x, uint16_t raw_y, uint8_t raw_f,
                                  uint16_t out_x, uint16_t out_y, uint8_t out_f)
{
    (void)ms;
    (void)raw_x;
    (void)raw_y;
    (void)raw_f;
    (void)out_x;
    (void)out_y;
    (void)out_f;
}
static inline void data_log_save_jpeg(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
}
static inline void data_log_suspend(void) {}
static inline esp_err_t data_log_resume(void) { return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_APP_PANEL_VARIANT_10INCH_JC */
