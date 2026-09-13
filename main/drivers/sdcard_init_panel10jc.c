/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * SDMMC microSD driver for the Guition JC8012P4A1C-I-W-Y 10.1" panel.
 *
 * The card slot is wired to SDMMC host slot 0 (IO MUX fixed pins:
 * CLK=43, CMD=44, D0=39, D1=40, D2=41, D3=42) with on-chip LDO (channel 4)
 * power control, 4-bit bus width and no card-detect / write-protect switches.
 * Mount point: /sdcard (FAT filesystem).
 */
#include "drivers/board_extras_panel10jc.h"

#include <string.h>

#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

#include "util/log_tags.h"

#define JC_SD_MOUNT_POINT "/sdcard"
#define JC_SD_LDO_CHANNEL 4

static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr_ctrl = NULL;
static SemaphoreHandle_t s_sd_mutex = NULL;

static SemaphoreHandle_t sdcard_mutex(void)
{
    if (s_sd_mutex == NULL) {
        s_sd_mutex = xSemaphoreCreateMutex();
    }
    return s_sd_mutex;
}

static void sdcard_free_pwr_ctrl(void)
{
    if (s_pwr_ctrl != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
    }
}

/* Mount the card.  When format_if_mount_failed is true, a card whose
 * filesystem is not recognized (e.g. an exFAT SDXC card, or a brand-new
 * unformatted card) is repartitioned and formatted to FAT32, then mounted.
 * When false, an unrecognized filesystem simply fails without touching data. */
static esp_err_t sdcard_mount_locked(bool format_if_mount_failed)
{
    if (s_card != NULL) {
        return ESP_OK;
    }

    const sdmmc_slot_config_t slot_config = {
        .cd = SDMMC_SLOT_NO_CD,
        .wp = SDMMC_SLOT_NO_WP,
        .width = 4,
        .flags = 0,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    /* Slot 0 is IO-MUX driven, so the GPIO matrix is not used. */
    host.flags &= ~SDMMC_HOST_FLAG_DDR;

    /* Power the SDMMC IO rail from the on-chip LDO (LDO_VO4). */
    const sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = JC_SD_LDO_CHANNEL,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SD, "sd_pwr_ctrl_new_on_chip_ldo failed: %s", esp_err_to_name(err));
        s_pwr_ctrl = NULL;
        return err;
    }
    host.pwr_ctrl_handle = s_pwr_ctrl;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_mount_failed,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    err = esp_vfs_fat_sdmmc_mount(JC_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG_SD, "mount failed (no card or unusable card): %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG_SD, "mount failed: %s", esp_err_to_name(err));
        }
        s_card = NULL;
        sdcard_free_pwr_ctrl();
        return err;
    }

    ESP_LOGI(TAG_SD, "microSD mounted at %s (name=%s, capacity=%llu bytes)",
             JC_SD_MOUNT_POINT,
             s_card->cid.name[0] != 0 ? (const char *)s_card->cid.name : "unknown",
             (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size));
    return ESP_OK;
}

static void sdcard_unmount_locked(void)
{
    if (s_card == NULL) {
        sdcard_free_pwr_ctrl();
        return;
    }
    esp_vfs_fat_sdcard_unmount(JC_SD_MOUNT_POINT, s_card);
    s_card = NULL;
    sdcard_free_pwr_ctrl();
}

esp_err_t sdcard_init(void)
{
    SemaphoreHandle_t m = sdcard_mutex();
    if (m == NULL) {
        ESP_LOGW(TAG_SD, "sdcard mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(m, portMAX_DELAY);
    esp_err_t err = sdcard_mount_locked(false);
    xSemaphoreGive(m);
    return err;
}

bool sdcard_is_ready(void)
{
    return s_card != NULL;
}

void sdcard_deinit(void)
{
    SemaphoreHandle_t m = sdcard_mutex();
    if (m == NULL) {
        return;
    }
    xSemaphoreTake(m, portMAX_DELAY);
    sdcard_unmount_locked();
    xSemaphoreGive(m);
}

void sdcard_unmount(void)
{
    sdcard_deinit();
}

esp_err_t sdcard_format_and_remount(void)
{
    SemaphoreHandle_t m = sdcard_mutex();
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(m, portMAX_DELAY);
    sdcard_unmount_locked();
    esp_err_t err = sdcard_mount_locked(true);
    xSemaphoreGive(m);
    return err;
}

esp_err_t sdcard_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (out_total_bytes == NULL && out_free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    esp_err_t err = esp_vfs_fat_info(JC_SD_MOUNT_POINT, &total, &free_bytes);
    if (err != ESP_OK) {
        return err;
    }

    if (out_total_bytes != NULL) {
        *out_total_bytes = total;
    }
    if (out_free_bytes != NULL) {
        *out_free_bytes = free_bytes;
    }
    return ESP_OK;
}
