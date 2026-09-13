/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * JC8012P4A1C-only board extras that have no upstream ESP-BSP package:
 * the ES8311 audio codec and the SDMMC microSD slot.  Both are optional
 * hardware, so every init entry point is best-effort and must never block
 * boot when the codec is not fitted or no card is inserted.
 */
#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared I2C bus (I2C_NUM_1: SCL=GPIO8, SDA=GPIO7) created by the touch
 * driver.  The ES8311 codec sits on the same bus, so audio_init() must be
 * called after touch_init(). */
i2c_master_bus_handle_t jc8012_i2c_bus_get(void);

/* ES8311 audio codec (speaker + microphone).  Fails gracefully with
 * ESP_ERR_INVALID_STATE if the shared I2C bus is not up yet. */
esp_err_t audio_init(void);
bool audio_is_ready(void);
void audio_deinit(void);

/* Raw codec-device handles for consumers that manage open/close themselves
 * (the Xiaozhi voice client).  Valid only after a successful audio_init(). */
esp_codec_dev_handle_t audio_get_speaker(void);
esp_codec_dev_handle_t audio_get_mic(void);

/* microSD card on SDMMC host slot 0 (IO MUX pins), mounted at /sdcard.
 * Fails gracefully when no card is inserted or the card cannot be mounted. */
esp_err_t sdcard_init(void);
bool sdcard_is_ready(void);
void sdcard_deinit(void);

/* Unmount the card without removing it (safe to call even when not mounted).
 * Used by the "format as new" flow before re-mounting with formatting. */
void sdcard_unmount(void);

/* Format the card as a fresh FAT32 volume and re-mount it at /sdcard.
 * This is the explicit "set the card up as new" action available from the
 * web Settings page.  All mount/unmount operations are serialized by an
 * internal mutex so this can never race the boot-time mount. */
esp_err_t sdcard_format_and_remount(void);

/* Report FAT volume capacity / free space.  Returns ESP_ERR_INVALID_STATE
 * when no card is mounted. */
esp_err_t sdcard_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

#ifdef __cplusplus
}
#endif
