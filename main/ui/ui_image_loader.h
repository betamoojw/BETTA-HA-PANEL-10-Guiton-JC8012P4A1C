/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Minimal synchronous image loader for tile backgrounds. Reads a PNG file
 * from the VFS (usually /sdcard/...) and decodes it into an LVGL-ready
 * RGB565 image scaled to exactly the requested target size.
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `path` (a PNG file reachable via stdio fopen) into `out`, scaling
 * the source to exactly target_w x target_h pixels using nearest-neighbour
 * sampling. Returns true on success; `out->data` is then owned by the caller
 * and must be released with heap_caps_free(). On failure `out` is left
 * untouched. */
bool ui_image_load_png_file(const char *path, int target_w, int target_h, lv_image_dsc_t *out);

/* Decode `path` (a JPEG file) into `out`, scaling the decoded frame to
 * exactly target_w x target_h pixels. Same ownership rules as above. */
bool ui_image_load_jpeg_file(const char *path, int target_w, int target_h, lv_image_dsc_t *out);

/* Format-agnostic dispatcher: detects PNG vs JPEG by magic bytes and decodes
 * into `out` at target_w x target_h. Prefer this for tile backgrounds. */
bool ui_image_load_file(const char *path, int target_w, int target_h, lv_image_dsc_t *out);

#ifdef __cplusplus
}
#endif
