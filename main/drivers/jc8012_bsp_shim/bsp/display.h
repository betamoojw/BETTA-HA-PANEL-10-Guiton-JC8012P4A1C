// SPDX-License-Identifier: LicenseRef-FNCL-1.1
// Copyright (c) 2026 Cpt_Kirk
//
// Minimal `bsp/display.h` shim for the Guition JC8012P4A1C panel.  The board
// has no upstream ESP-BSP package, but a few app sources (api_settings.c,
// api_ota.c) include this header and call bsp_display_backlight_off().  The
// implementation lives in drivers/display_init_panel10jc.c.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_display_backlight_off(void);

#ifdef __cplusplus
}
#endif
