/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Built-in OV02C10 MIPI-CSI camera pipeline for the JC8012P4A1C panel.
 *
 * Ported from the official Guition video_lcd_display demo (app_video.c) and
 * extended with a latest-frame cache + hardware JPEG snapshot + motion wake.
 */
#include "camera/local_camera.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"

#include "app_config.h"
#include "drivers/board_extras_panel10jc.h"
#include "drivers/display_init.h"
#include "util/log_tags.h"

#define LOCAL_CAMERA_BUF_COUNT        2
#define LOCAL_CAMERA_STACK_SIZE       (5 * 1024)
#define LOCAL_CAMERA_TASK_PRIORITY    5
#define LOCAL_CAMERA_TASK_CORE        0

/* Motion detector: sample a 32x32 luminance grid and compare against the
 * previous frame.  Fires the wake callback when the mean absolute difference
 * exceeds LOCAL_CAMERA_MOTION_THRESHOLD, throttled to once per second. */
#define LOCAL_CAMERA_MOTION_GRID       32
#define LOCAL_CAMERA_MOTION_THRESHOLD  8
#define LOCAL_CAMERA_MOTION_COOLDOWN_MS 1000

typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    size_t buf_size;

    uint8_t *capture_bufs[LOCAL_CAMERA_BUF_COUNT];
    uint8_t *latest_frame;
    bool have_frame;
    SemaphoreHandle_t frame_mutex;

    jpeg_encoder_handle_t jpeg_engine;
    uint8_t *jpeg_out;
    size_t jpeg_out_size;

    TaskHandle_t task;
    volatile bool stop_requested;

    bool motion_wake_enabled;
    uint8_t motion_threshold;
    uint8_t jpeg_quality;
    bool hflip;
    bool vflip;
    uint8_t *prev_luma;
    uint32_t frame_counter;
    int64_t last_motion_ms;
    local_camera_motion_cb_t motion_cb;
    void *motion_user;
} local_camera_t;

static local_camera_t s_cam = {
    .fd = -1,
};

static int64_t now_ms(void)
{
    return (int64_t)esp_timer_get_time() / 1000;
}

static void set_flip(bool hflip, bool vflip)
{
    if (s_cam.fd < 0) {
        return;
    }

    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control[2];

    memset(&controls, 0, sizeof(controls));
    memset(control, 0, sizeof(control));

    /* Always send both controls so a flip can be disabled at runtime without
     * a full pipeline restart. */
    control[0].id = V4L2_CID_HFLIP;
    control[0].value = hflip ? 1 : 0;
    control[1].id = V4L2_CID_VFLIP;
    control[1].value = vflip ? 1 : 0;

    controls.ctrl_class = V4L2_CTRL_CLASS_USER;
    controls.count = 2;
    controls.controls = control;
    if (ioctl(s_cam.fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG_CAMERA, "Failed to apply camera flip (%d/%d)", hflip, vflip);
    }
}

static uint8_t rgb565_luma(uint16_t px)
{
    const uint32_t r = (px >> 11) & 0x1FU;
    const uint32_t g = (px >> 5) & 0x3FU;
    const uint32_t b = px & 0x1FU;
    /* 0..31 for r/b, 0..63 for g: scale to a rough 0..255 luma. */
    return (uint8_t)(((r * 30U) + (g * 30U) + (b * 11U)) >> 6);
}

static void motion_detect(const uint8_t *frame)
{
    if (s_cam.prev_luma == NULL || frame == NULL) {
        return;
    }

    const uint32_t step_x = s_cam.width / LOCAL_CAMERA_MOTION_GRID;
    const uint32_t step_y = s_cam.height / LOCAL_CAMERA_MOTION_GRID;
    if (step_x == 0 || step_y == 0) {
        return;
    }

    uint32_t diff_sum = 0;
    uint8_t *prev = s_cam.prev_luma;
    for (uint32_t gy = 0; gy < LOCAL_CAMERA_MOTION_GRID; gy++) {
        const uint32_t y = (gy * step_y) + (step_y / 2);
        const uint8_t *row = frame + ((size_t)y * s_cam.width * 2);
        for (uint32_t gx = 0; gx < LOCAL_CAMERA_MOTION_GRID; gx++) {
            const uint32_t x = (gx * step_x) + (step_x / 2);
            const uint8_t luma = rgb565_luma((uint16_t)(row[x * 2] | (row[x * 2 + 1] << 8)));
            const uint8_t old = *prev;
            *prev = luma;
            diff_sum += (luma > old) ? (luma - old) : (old - luma);
            prev++;
        }
    }

    const uint32_t mean_diff = diff_sum / (LOCAL_CAMERA_MOTION_GRID * LOCAL_CAMERA_MOTION_GRID);
    if (mean_diff < s_cam.motion_threshold) {
        return;
    }

    const int64_t now = now_ms();
    if (now - s_cam.last_motion_ms < LOCAL_CAMERA_MOTION_COOLDOWN_MS) {
        return;
    }
    s_cam.last_motion_ms = now;

    if (s_cam.motion_cb != NULL) {
        s_cam.motion_cb(s_cam.motion_user);
    }
}

static void stream_task(void *arg)
{
    (void)arg;

    while (!s_cam.stop_requested) {
        struct v4l2_buffer vb;
        memset(&vb, 0, sizeof(vb));
        vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        vb.memory = V4L2_MEMORY_USERPTR;

        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &vb) != 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        s_cam.frame_counter++;

        /* Copy every 2nd frame to the latest-frame cache so the JPEG snapshot
         * does not contend with the DMA buffers. */
        if ((s_cam.frame_counter & 1U) == 0U) {
            if (xSemaphoreTake(s_cam.frame_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(s_cam.latest_frame, s_cam.capture_bufs[vb.index], s_cam.buf_size);
                s_cam.have_frame = true;
                xSemaphoreGive(s_cam.frame_mutex);
            }
        }

        if (s_cam.motion_wake_enabled && (s_cam.frame_counter % 4U) == 0U) {
            motion_detect(s_cam.capture_bufs[vb.index]);
        }

        if (ioctl(s_cam.fd, VIDIOC_QBUF, &vb) != 0) {
            ESP_LOGW(TAG_CAMERA, "Failed to requeue camera buffer");
        }
    }

    vTaskDelete(NULL);
}

static esp_err_t open_device(void)
{
    const char *dev = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;
    int fd = open(dev, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG_CAMERA, "Open %s failed", dev);
        return ESP_FAIL;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG_CAMERA, "VIDIOC_G_FMT failed");
        close(fd);
        return ESP_FAIL;
    }

    s_cam.width = fmt.fmt.pix.width;
    s_cam.height = fmt.fmt.pix.height;

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        struct v4l2_format request = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .fmt.pix.width = fmt.fmt.pix.width,
            .fmt.pix.height = fmt.fmt.pix.height,
            .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
        };
        if (ioctl(fd, VIDIOC_S_FMT, &request) != 0) {
            ESP_LOGE(TAG_CAMERA, "VIDIOC_S_FMT RGB565 failed");
            close(fd);
            return ESP_FAIL;
        }
    }

    s_cam.buf_size = (size_t)s_cam.width * s_cam.height * 2;
    s_cam.fd = fd;

    ESP_LOGI(TAG_CAMERA, "Camera opened: %" PRIu32 "x%" PRIu32 " RGB565 (%u bytes/frame)",
             s_cam.width, s_cam.height, (unsigned)s_cam.buf_size);

    return ESP_OK;
}

static esp_err_t setup_buffers(void)
{
    /* ESP32-P4 data cache line size; USERPTR DMA buffers must be cache-line aligned. */
    const size_t cache_line = 64;

    for (int i = 0; i < LOCAL_CAMERA_BUF_COUNT; i++) {
        s_cam.capture_bufs[i] = heap_caps_aligned_calloc(cache_line, 1, s_cam.buf_size, MALLOC_CAP_SPIRAM);
        if (s_cam.capture_bufs[i] == NULL) {
            ESP_LOGE(TAG_CAMERA, "Failed to allocate capture buffer %d (%u bytes)", i, (unsigned)s_cam.buf_size);
            return ESP_ERR_NO_MEM;
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = LOCAL_CAMERA_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;
    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG_CAMERA, "VIDIOC_REQBUFS failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < LOCAL_CAMERA_BUF_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;
        buf.index = i;

        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG_CAMERA, "VIDIOC_QUERYBUF %d failed", i);
            return ESP_FAIL;
        }
        if (buf.length > s_cam.buf_size) {
            ESP_LOGE(TAG_CAMERA, "Driver wants %u bytes but only %u allocated", buf.length, (unsigned)s_cam.buf_size);
            return ESP_ERR_NO_MEM;
        }

        buf.m.userptr = (unsigned long)s_cam.capture_bufs[i];
        buf.length = s_cam.buf_size;
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG_CAMERA, "VIDIOC_QBUF %d failed", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static esp_err_t setup_jpeg(void)
{
    jpeg_encode_engine_cfg_t eng_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    if (jpeg_new_encoder_engine(&eng_cfg, &s_cam.jpeg_engine) != ESP_OK) {
        ESP_LOGE(TAG_CAMERA, "jpeg_new_encoder_engine failed");
        return ESP_FAIL;
    }

    jpeg_encode_memory_alloc_cfg_t out_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t allocated = 0;
    /* RGB565 -> JPEG at quality <= 95 compresses well under 4:1. */
    const size_t want = (s_cam.buf_size / 4) + 4096;
    s_cam.jpeg_out = jpeg_alloc_encoder_mem(want, &out_cfg, &allocated);
    if (s_cam.jpeg_out == NULL) {
        ESP_LOGE(TAG_CAMERA, "jpeg_alloc_encoder_mem failed");
        return ESP_ERR_NO_MEM;
    }
    s_cam.jpeg_out_size = allocated;
    return ESP_OK;
}

esp_err_t local_camera_start(void)
{
    if (s_cam.task != NULL) {
        return ESP_OK;
    }

    esp_video_init_csi_config_t csi_config[] = {
        {
            .sccb_config = {
                .init_sccb = true,
                .i2c_config = {
                    .port = 0,
                    .scl_pin = GPIO_NUM_8,
                    .sda_pin = GPIO_NUM_7,
                },
                .freq = 100000,
            },
            .reset_pin = -1,
            .pwdn_pin = -1,
        },
    };

    i2c_master_bus_handle_t bus = jc8012_i2c_bus_get();
    if (bus != NULL) {
        csi_config[0].sccb_config.init_sccb = false;
        csi_config[0].sccb_config.i2c_handle = bus;
    }

    esp_video_init_config_t cam_config = {
        .csi = csi_config,
    };
    esp_err_t err = esp_video_init(&cam_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_CAMERA, "esp_video_init failed: %s", esp_err_to_name(err));
        return err;
    }

    if (open_device() != ESP_OK) {
        goto fail;
    }

    s_cam.frame_mutex = xSemaphoreCreateMutex();
    if (s_cam.frame_mutex == NULL) {
        ESP_LOGE(TAG_CAMERA, "Failed to create frame mutex");
        goto fail;
    }

    const size_t cache_line = 64;
    s_cam.latest_frame = heap_caps_aligned_calloc(cache_line, 1, s_cam.buf_size, MALLOC_CAP_SPIRAM);
    if (s_cam.latest_frame == NULL) {
        ESP_LOGE(TAG_CAMERA, "Failed to allocate latest-frame buffer");
        goto fail;
    }
    s_cam.prev_luma = heap_caps_calloc(LOCAL_CAMERA_MOTION_GRID * LOCAL_CAMERA_MOTION_GRID, 1, MALLOC_CAP_INTERNAL);
    if (s_cam.prev_luma == NULL) {
        ESP_LOGE(TAG_CAMERA, "Failed to allocate motion grid");
        goto fail;
    }

    if (setup_buffers() != ESP_OK) {
        goto fail;
    }
    if (setup_jpeg() != ESP_OK) {
        goto fail;
    }

    s_cam.motion_wake_enabled = CONFIG_APP_LOCAL_CAMERA_MOTION_WAKE;
    s_cam.motion_threshold = LOCAL_CAMERA_MOTION_THRESHOLD;
    s_cam.jpeg_quality = CONFIG_APP_LOCAL_CAMERA_JPEG_QUALITY;
#ifdef CONFIG_APP_LOCAL_CAMERA_HFLIP
    s_cam.hflip = CONFIG_APP_LOCAL_CAMERA_HFLIP;
#else
    s_cam.hflip = false;
#endif
#ifdef CONFIG_APP_LOCAL_CAMERA_VFLIP
    s_cam.vflip = CONFIG_APP_LOCAL_CAMERA_VFLIP;
#else
    s_cam.vflip = false;
#endif
    set_flip(s_cam.hflip, s_cam.vflip);

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG_CAMERA, "VIDIOC_STREAMON failed");
        goto fail;
    }

    s_cam.stop_requested = false;
    if (xTaskCreatePinnedToCore(stream_task, "local_camera", LOCAL_CAMERA_STACK_SIZE, NULL,
                                LOCAL_CAMERA_TASK_PRIORITY, &s_cam.task, LOCAL_CAMERA_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG_CAMERA, "Failed to create stream task");
        s_cam.task = NULL;
        goto fail;
    }

    ESP_LOGI(TAG_CAMERA, "Local camera started (%" PRIu32 "x%" PRIu32 ")", s_cam.width, s_cam.height);
    return ESP_OK;

fail:
    local_camera_deinit();
    return ESP_FAIL;
}

esp_err_t local_camera_stop(void)
{
    if (s_cam.task == NULL) {
        return ESP_OK;
    }

    s_cam.stop_requested = true;
    /* Give the stream task a moment to observe the flag and STREAMOFF. */
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s_cam.fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_cam.fd, VIDIOC_STREAMOFF, &type);
    }

    /* The task deletes itself; clear the handle. */
    s_cam.task = NULL;
    return ESP_OK;
}

void local_camera_deinit(void)
{
    local_camera_stop();

    if (s_cam.jpeg_engine != NULL) {
        jpeg_del_encoder_engine(s_cam.jpeg_engine);
        s_cam.jpeg_engine = NULL;
    }
    if (s_cam.jpeg_out != NULL) {
        free(s_cam.jpeg_out);
        s_cam.jpeg_out = NULL;
        s_cam.jpeg_out_size = 0;
    }
    if (s_cam.latest_frame != NULL) {
        heap_caps_free(s_cam.latest_frame);
        s_cam.latest_frame = NULL;
    }
    if (s_cam.prev_luma != NULL) {
        heap_caps_free(s_cam.prev_luma);
        s_cam.prev_luma = NULL;
    }
    for (int i = 0; i < LOCAL_CAMERA_BUF_COUNT; i++) {
        if (s_cam.capture_bufs[i] != NULL) {
            heap_caps_free(s_cam.capture_bufs[i]);
            s_cam.capture_bufs[i] = NULL;
        }
    }
    if (s_cam.frame_mutex != NULL) {
        vSemaphoreDelete(s_cam.frame_mutex);
        s_cam.frame_mutex = NULL;
    }
    if (s_cam.fd >= 0) {
        close(s_cam.fd);
        s_cam.fd = -1;
    }
    s_cam.have_frame = false;
}

bool local_camera_is_running(void)
{
    return s_cam.task != NULL;
}

void local_camera_set_motion_wake(bool enabled)
{
    s_cam.motion_wake_enabled = enabled;
}

void local_camera_set_motion_threshold(uint8_t threshold)
{
    if (threshold < 1) {
        threshold = 1;
    }
    if (threshold > 64) {
        threshold = 64;
    }
    s_cam.motion_threshold = threshold;
}

void local_camera_set_jpeg_quality(uint8_t quality)
{
    if (quality < 10) {
        quality = 10;
    }
    if (quality > 95) {
        quality = 95;
    }
    s_cam.jpeg_quality = quality;
}

void local_camera_set_flip(bool hflip, bool vflip)
{
    s_cam.hflip = hflip;
    s_cam.vflip = vflip;
    set_flip(hflip, vflip);
}

bool local_camera_get_motion_wake(void)
{
    return s_cam.motion_wake_enabled;
}

uint8_t local_camera_get_motion_threshold(void)
{
    return s_cam.motion_threshold;
}

uint8_t local_camera_get_jpeg_quality(void)
{
    return s_cam.jpeg_quality;
}

bool local_camera_get_hflip(void)
{
    return s_cam.hflip;
}

bool local_camera_get_vflip(void)
{
    return s_cam.vflip;
}

esp_err_t local_camera_snapshot_jpeg(uint8_t **out_buf, size_t *out_len)
{
    if (out_buf == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    if (!local_camera_is_running() || !s_cam.have_frame) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_cam.frame_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    jpeg_encode_cfg_t cfg = {
        .height = s_cam.height,
        .width = s_cam.width,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = s_cam.jpeg_quality,
        .pixel_reverse = false,
    };

    uint32_t out_size = 0;
    esp_err_t err = jpeg_encoder_process(s_cam.jpeg_engine, &cfg, s_cam.latest_frame, s_cam.buf_size,
                                         s_cam.jpeg_out, s_cam.jpeg_out_size, &out_size);
    xSemaphoreGive(s_cam.frame_mutex);

    if (err != ESP_OK || out_size == 0) {
        ESP_LOGE(TAG_CAMERA, "JPEG encode failed: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    uint8_t *copy = malloc(out_size);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, s_cam.jpeg_out, out_size);
    *out_buf = copy;
    *out_len = out_size;
    return ESP_OK;
}

esp_err_t local_camera_register_motion_cb(local_camera_motion_cb_t cb, void *user_data)
{
    s_cam.motion_cb = cb;
    s_cam.motion_user = user_data;
    return ESP_OK;
}

int local_camera_width(void)
{
    return (int)s_cam.width;
}

int local_camera_height(void)
{
    return (int)s_cam.height;
}
