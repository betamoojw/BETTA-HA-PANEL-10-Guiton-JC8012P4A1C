/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Built-in OV02C10 MIPI-CSI camera of the Guition JC8012P4A1C panel.
 *
 * The sensor driver (esp_cam_sensor 2.1.0) is vendored under components/ and
 * pulled in by the ESP Video framework (esp_video ~2.0).  This module owns the
 * V4L2 capture pipeline (/dev/video0): continuous RGB565 capture into PSRAM
 * USERPTR buffers, a mutex-guarded "latest frame" cache, a lightweight
 * frame-difference motion detector for screen wake, and on-demand hardware
 * JPEG encoding for the web snapshot endpoint.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*local_camera_motion_cb_t)(void *user_data);

/* Start the camera pipeline.  Reuses the touch/codec I2C bus (I2C_NUM_1,
 * SCL=8/SDA=7) for the sensor SCCB interface.  Safe to call repeatedly. */
esp_err_t local_camera_start(void);

/* Stop the stream task and release the video device.  Keep allocated frame
 * buffers so a later start() is cheap; use local_camera_deinit() to free. */
esp_err_t local_camera_stop(void);

/* Full teardown (stop + free buffers/JPEG engine). */
void local_camera_deinit(void);

bool local_camera_is_running(void);

/* Enable/disable the motion-wake detector (display_note_activity on motion). */
void local_camera_set_motion_wake(bool enabled);

/* Runtime motion sensitivity: mean-absolute-difference threshold over the
 * 32x32 luma grid.  Higher values = less sensitive.  Range 1..64. */
void local_camera_set_motion_threshold(uint8_t threshold);

/* Runtime JPEG quality used by local_camera_snapshot_jpeg().  Range 10..95. */
void local_camera_set_jpeg_quality(uint8_t quality);

/* Runtime mirroring.  Applied immediately if the pipeline is running. */
void local_camera_set_flip(bool hflip, bool vflip);

/* Encode the latest captured frame to JPEG and return a freshly allocated
 * buffer.  The caller owns *out_buf and must free() it. */
esp_err_t local_camera_snapshot_jpeg(uint8_t **out_buf, size_t *out_len);

/* Register a callback fired when motion is detected. */
esp_err_t local_camera_register_motion_cb(local_camera_motion_cb_t cb, void *user_data);

bool local_camera_get_motion_wake(void);
uint8_t local_camera_get_motion_threshold(void);
uint8_t local_camera_get_jpeg_quality(void);
bool local_camera_get_hflip(void);
bool local_camera_get_vflip(void);

int local_camera_width(void);
int local_camera_height(void);

#ifdef __cplusplus
}
#endif
