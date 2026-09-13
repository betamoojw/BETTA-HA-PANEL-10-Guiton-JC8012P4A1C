/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t touch_init(void);
bool touch_is_ready(void);
/* Enable/disable the touch debug overlay (red dots + serial log). No-op on
 * panels without the debug overlay implementation. */
void touch_debug_set_enabled(bool enabled);
