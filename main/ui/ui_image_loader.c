/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Synchronous PNG file -> LVGL RGB565 image loader for tile backgrounds.
 * Runs on the LVGL task during layout build; a background image is decoded
 * once per tile and kept for the lifetime of the widget.
 */
#include "ui/ui_image_loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "soc/soc_caps.h"

#if SOC_JPEG_DECODE_SUPPORTED
#include "driver/jpeg_decode.h"
#endif

#include "../../managed_components/lvgl__lvgl/src/libs/lodepng/lodepng.h"

#define TAG "ui_image_loader"

/* Hard cap for the decoded source file; backgrounds are tile-sized, so a
 * couple of MiB is more than enough for even a full-screen 1024x600 PNG. */
#define UI_IMAGE_MAX_FILE_BYTES (4U * 1024U * 1024U)

/* Maximum decoded pixel budget. The image is downscaled to the panel anyway,
 * so accepting sources bigger than ~1.4x the panel resolution only wastes
 * PSRAM. During decode lodepng holds the raw scanlines AND an ARGB8888 copy
 * at once (8 bytes/pixel) on top of the compressed file, so a 1920x1080+
 * wallpaper would exhaust the ~15 MB of PSRAM and fail with lodepng error 83
 * (alloc fail). 1.2 MP covers up to ~1366x768 / 1280x960. */
#define UI_IMAGE_MAX_DECODED_PIXELS (1200000U)

/* JPEG HW decoder cannot downscale, so the full frame is decoded first. Cap
 * the source at 1.5 MP (≈1280x1200) to keep the RGB565 decode buffer sane. */
#define UI_IMAGE_MAX_JPEG_PIXELS (1500000U)

static bool ui_image_read_file(const char *path, uint8_t **out_buf, size_t *out_len)
{
    if (path == NULL || out_buf == NULL || out_len == NULL) {
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0 || (unsigned long)size > UI_IMAGE_MAX_FILE_BYTES) {
        ESP_LOGW(TAG, "bad size %ld for %s", size, path);
        fclose(f);
        return false;
    }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return false;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        return false;
    }

    *out_buf = buf;
    *out_len = (size_t)size;
    return true;
}

/* LVGL's modified lodepng returns an ARGB8888 lv_draw_buf_t, but the raw
 * bytes lodepng writes are in R,G,B,A order (getPixelColorsRGBA8). LVGL's own
 * lv_lodepng.c swaps red/blue before use; we skip the swap and pack the
 * channels in the correct order directly into RGB565. */
static bool ui_image_scale_argb8888_to_rgb565(const uint8_t *src_argb,
                                              uint32_t src_w,
                                              uint32_t src_h,
                                              uint32_t dst_w,
                                              uint32_t dst_h,
                                              uint16_t *dst)
{
    if (src_argb == NULL || dst == NULL || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return false;
    }

    const uint32_t src_bpp = 4; /* ARGB8888 */
    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        const uint8_t *srow = src_argb + (size_t)src_y * src_w * src_bpp;
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / dst_w);
            const uint8_t *p = srow + (size_t)src_x * src_bpp;
            /* lodepng RGBA8 byte order: p[0]=red, p[1]=green, p[2]=blue, p[3]=alpha */
            dst[(size_t)y * dst_w + x] =
                (uint16_t)(((p[0] & 0xF8U) << 8) | ((p[1] & 0xFCU) << 3) | (p[2] >> 3));
        }
    }
    return true;
}

bool ui_image_load_png_file(const char *path, int target_w, int target_h, lv_image_dsc_t *out)
{
    if (path == NULL || path[0] == '\0' || out == NULL || target_w < 1 || target_h < 1) {
        return false;
    }

    uint8_t *file_buf = NULL;
    size_t file_len = 0;
    if (!ui_image_read_file(path, &file_buf, &file_len)) {
        return false;
    }

    /* Read only the IHDR first: reject oversized sources before lodepng
     * allocates the raw scanlines + ARGB8888 working copy. */
    unsigned iw = 0;
    unsigned ih = 0;
    LodePNGState inspect_state;
    lodepng_state_init(&inspect_state);
    unsigned inspect_err = lodepng_inspect(&iw, &ih, &inspect_state, file_buf, file_len);
    unsigned bpp = lodepng_get_bpp(&inspect_state.info_png.color);
    lodepng_state_cleanup(&inspect_state);
    if (inspect_err != 0 || iw == 0 || ih == 0) {
        ESP_LOGW(TAG, "PNG header invalid (%u) for %s", inspect_err, path);
        free(file_buf);
        return false;
    }
    uint64_t pixels = (uint64_t)iw * (uint64_t)ih;
    /* Decode peak has two phases (see lodepng.c decodeGeneric):
     *   phase A (inflate): loader buffer + idat copy + raw scanlines
     *   phase B (post-process): loader buffer + scanlines + ARGB8888 working copy
     * idat is freed before the ARGB8888 buffer is allocated, so the peak is
     * the larger of the two phases, not their sum. Compare against currently
     * free PSRAM with a small fragmentation headroom so we reject the source
     * BEFORE lodepng runs out of memory (its cryptic error 83). */
    uint64_t bytes_per_px = ((uint64_t)bpp + 7U) / 8U; /* round up sub-byte gray depths */
    uint64_t peak_a = 2ULL * (uint64_t)file_len + pixels * bytes_per_px;
    uint64_t peak_b = (uint64_t)file_len + pixels * (bytes_per_px + 4ULL);
    uint64_t peak = (peak_a > peak_b) ? peak_a : peak_b;
    size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t headroom = 512U * 1024U;
    size_t budget = (free_spiram > headroom) ? (free_spiram - headroom) : 0;
    if (pixels > UI_IMAGE_MAX_DECODED_PIXELS || peak > (uint64_t)budget) {
        ESP_LOGW(TAG, "PNG too large (%u x %u, %zu bytes) for %s; skipping (peak %llu > free %zu)",
                 iw, ih, file_len, path, peak, free_spiram);
        free(file_buf);
        return false;
    }

    unsigned decoded_w = 0;
    unsigned decoded_h = 0;
    lv_draw_buf_t *decoded = NULL;
    unsigned err = lodepng_decode32((unsigned char **)&decoded, &decoded_w, &decoded_h, file_buf, file_len);
    free(file_buf);
    if (err != 0 || decoded == NULL || decoded->data == NULL || decoded_w == 0 || decoded_h == 0) {
        ESP_LOGW(TAG, "PNG decode failed (%u) for %s", err, path);
        if (decoded != NULL) {
            lv_draw_buf_destroy(decoded);
        }
        return false;
    }

    /* Sanity-cap the decoded dimensions so a corrupt/oversized PNG can never
     * make the scaling loop overflow the ARGB8888 source buffer. */
    if (decoded_w > 8192U || decoded_h > 8192U) {
        ESP_LOGW(TAG, "PNG too large (%u x %u) for %s", decoded_w, decoded_h, path);
        lv_draw_buf_destroy(decoded);
        return false;
    }

    uint32_t dst_w = (uint32_t)target_w;
    uint32_t dst_h = (uint32_t)target_h;
    uint32_t stride = lv_draw_buf_width_to_stride(dst_w, LV_COLOR_FORMAT_RGB565);
    size_t dst_size = (size_t)stride * dst_h;

    uint16_t *dst = (uint16_t *)heap_caps_malloc(dst_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst == NULL) {
        dst = (uint16_t *)heap_caps_malloc(dst_size, MALLOC_CAP_8BIT);
    }
    if (dst == NULL) {
        ESP_LOGE(TAG, "OOM allocating %u x %u RGB565 (%u bytes)", (unsigned)dst_w, (unsigned)dst_h,
                 (unsigned)dst_size);
        lv_draw_buf_destroy(decoded);
        return false;
    }

    /* Write into a stride-aligned buffer; nearest scale fills width dst_w,
     * leaving any stride padding bytes untouched (RGB565 stride == w*2 so
     * there is normally no padding). */
    memset(dst, 0, dst_size);
    ui_image_scale_argb8888_to_rgb565(decoded->data, decoded_w, decoded_h, dst_w, dst_h, dst);
    lv_draw_buf_destroy(decoded);

    out->header.magic = LV_IMAGE_HEADER_MAGIC;
    out->header.cf = LV_COLOR_FORMAT_RGB565;
    out->header.w = dst_w;
    out->header.h = dst_h;
    out->header.stride = stride;
    out->data = (const uint8_t *)dst;
    out->data_size = dst_size;
    return true;
}

#if SOC_JPEG_DECODE_SUPPORTED
static jpeg_decoder_handle_t s_jpeg_decoder = NULL;

static bool ui_image_jpeg_engine_ready(void)
{
    if (s_jpeg_decoder != NULL) {
        return true;
    }
    jpeg_decode_engine_cfg_t eng = {
        .intr_priority = 0,
        .timeout_ms = 2000,
    };
    if (jpeg_new_decoder_engine(&eng, &s_jpeg_decoder) != ESP_OK) {
        ESP_LOGW(TAG, "jpeg_new_decoder_engine failed");
        return false;
    }
    return true;
}
#endif

/* Nearest-neighbour RGB565 scale. The JPEG HW decoder emits full source
 * resolution, so we downscale to the tile size before handing the image to
 * LVGL (keeps the retained buffer small, mirroring the PNG path). */
#if SOC_JPEG_DECODE_SUPPORTED
static void ui_image_scale_rgb565_nearest(const uint16_t *src,
                                          uint32_t src_w,
                                          uint32_t src_h,
                                          uint32_t dst_w,
                                          uint32_t dst_h,
                                          uint16_t *dst)
{
    if (src == NULL || dst == NULL || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return;
    }
    for (uint32_t y = 0; y < dst_h; y++) {
        uint32_t src_y = (uint32_t)(((uint64_t)y * src_h) / dst_h);
        const uint16_t *srow = src + (size_t)src_y * src_w;
        for (uint32_t x = 0; x < dst_w; x++) {
            uint32_t src_x = (uint32_t)(((uint64_t)x * src_w) / dst_w);
            dst[(size_t)y * dst_w + x] = srow[src_x];
        }
    }
}
#endif /* SOC_JPEG_DECODE_SUPPORTED */

bool ui_image_load_jpeg_file(const char *path, int target_w, int target_h, lv_image_dsc_t *out)
{
#if !SOC_JPEG_DECODE_SUPPORTED
    (void)path;
    (void)target_w;
    (void)target_h;
    (void)out;
    ESP_LOGW(TAG, "JPEG decode not supported on this target");
    return false;
#else
    if (path == NULL || path[0] == '\0' || out == NULL || target_w < 1 || target_h < 1) {
        return false;
    }

    uint8_t *file_buf = NULL;
    size_t file_len = 0;
    if (!ui_image_read_file(path, &file_buf, &file_len)) {
        return false;
    }

    jpeg_decode_picture_info_t pic = {0};
    if (jpeg_decoder_get_info(file_buf, file_len, &pic) != ESP_OK || pic.width == 0 || pic.height == 0) {
        ESP_LOGW(TAG, "JPEG header invalid for %s", path);
        free(file_buf);
        return false;
    }

    uint64_t pixels = (uint64_t)pic.width * (uint64_t)pic.height;
    if (pixels > UI_IMAGE_MAX_JPEG_PIXELS) {
        ESP_LOGW(TAG, "JPEG too large (%u x %u) for %s", (unsigned)pic.width, (unsigned)pic.height,
                 path);
        free(file_buf);
        return false;
    }

    uint32_t dec_w = (pic.width + 15U) & ~15U;
    uint32_t dec_h = (pic.height + 15U) & ~15U;
    size_t rgb_size = (size_t)dec_w * dec_h * 2;

    if (!ui_image_jpeg_engine_ready()) {
        free(file_buf);
        return false;
    }

    jpeg_decode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated = 0;
    uint8_t *rgb = jpeg_alloc_decoder_mem(rgb_size, &mem_cfg, &allocated);
    if (rgb == NULL) {
        ESP_LOGW(TAG, "jpeg alloc output failed for %s", path);
        free(file_buf);
        return false;
    }

    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t produced = 0;
    esp_err_t err = jpeg_decoder_process(s_jpeg_decoder, &dcfg, file_buf, file_len, rgb, allocated,
                                         &produced);
    free(file_buf);
    if (err != ESP_OK || produced == 0) {
        ESP_LOGW(TAG, "JPEG decode failed (%s) for %s", esp_err_to_name(err), path);
        heap_caps_free(rgb);
        return false;
    }

    uint32_t dst_w = (uint32_t)target_w;
    uint32_t dst_h = (uint32_t)target_h;
    uint32_t stride = lv_draw_buf_width_to_stride(dst_w, LV_COLOR_FORMAT_RGB565);
    size_t dst_size = (size_t)stride * dst_h;

    uint16_t *dst = (uint16_t *)heap_caps_malloc(dst_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dst == NULL) {
        dst = (uint16_t *)heap_caps_malloc(dst_size, MALLOC_CAP_8BIT);
    }
    if (dst == NULL) {
        ESP_LOGE(TAG, "OOM allocating RGB565 %ux%u", (unsigned)dst_w, (unsigned)dst_h);
        heap_caps_free(rgb);
        return false;
    }

    memset(dst, 0, dst_size);
    ui_image_scale_rgb565_nearest((const uint16_t *)rgb, dec_w, dec_h, dst_w, dst_h, dst);
    heap_caps_free(rgb);

    out->header.magic = LV_IMAGE_HEADER_MAGIC;
    out->header.cf = LV_COLOR_FORMAT_RGB565;
    out->header.w = dst_w;
    out->header.h = dst_h;
    out->header.stride = stride;
    out->data = (const uint8_t *)dst;
    out->data_size = dst_size;
    return true;
#endif
}

static bool ui_image_has_png_magic(const uint8_t *buf, size_t len)
{
    return len >= 8 && buf[0] == 0x89 && buf[1] == 0x50 && buf[2] == 0x4E && buf[3] == 0x47 &&
           buf[4] == 0x0D && buf[5] == 0x0A && buf[6] == 0x1A && buf[7] == 0x0A;
}

static bool ui_image_has_jpeg_magic(const uint8_t *buf, size_t len)
{
    return len >= 2 && buf[0] == 0xFF && buf[1] == 0xD8;
}

bool ui_image_load_file(const char *path, int target_w, int target_h, lv_image_dsc_t *out)
{
    if (path == NULL || path[0] == '\0' || out == NULL) {
        return false;
    }

    /* Peek at the first bytes to dispatch to the right decoder. */
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return false;
    }
    uint8_t magic[8] = {0};
    size_t got = fread(magic, 1, sizeof(magic), f);
    fclose(f);

    if (ui_image_has_png_magic(magic, got)) {
        return ui_image_load_png_file(path, target_w, target_h, out);
    }
    if (ui_image_has_jpeg_magic(magic, got)) {
        return ui_image_load_jpeg_file(path, target_w, target_h, out);
    }

    ESP_LOGW(TAG, "unsupported image format for %s", path);
    return false;
}
