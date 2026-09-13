/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Touch driver for the Guition JC8012P4A1C-I-W-Y 10.1" panel.
 *
 * The touch controller is a GSL3680 on I2C_NUM_1 (SCL=GPIO8, SDA=GPIO7,
 * 400 kHz, device address 0x40).  Unlike the Waveshare variants there is no
 * upstream BSP, so the I2C bus and touch handle are created directly here.
 * The driver source (esp_lcd_touch_gsl3680) comes from the official Guition
 * reference for this panel.
 */
#include "drivers/touch_init.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gsl3680.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "drivers/display_init.h"
#include "drivers/board_extras_panel10jc.h"
#include "diag/data_log.h"
#include "util/log_tags.h"

#define JC_TOUCH_I2C_PORT I2C_NUM_1
#define JC_TOUCH_I2C_SCL GPIO_NUM_8
#define JC_TOUCH_I2C_SDA GPIO_NUM_7
#define JC_TOUCH_I2C_CLK_HZ 400000
#define JC_TOUCH_RST_GPIO GPIO_NUM_22
#define JC_TOUCH_INT_GPIO GPIO_NUM_21
/* The GSL3680 driver outputs coordinates already in native portrait ranges:
 * x = panel short axis 0..800, y = panel long axis 0..1280 (verified from
 * live raw capture: x max ~795, y max ~1166). LVGL rotates indev points
 * 270 deg itself, so no swap is needed — only mirror the X axis, because on
 * this panel's mounting the raw touch axes are 180 deg inverted relative to
 * the display (see ANALIZA-NAPRAWA-DOTYKU.md). */
#define JC_TOUCH_X_MAX 800
#define JC_TOUCH_Y_MAX 1280

static bool s_touch_ready = false;
static lv_indev_t *s_touch_indev = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static const int TOUCH_INIT_RETRIES = 8;
static const int TOUCH_INIT_RETRY_DELAY_MS = 250;
static const uint32_t TOUCH_POLL_PERIOD_MS = 10;

/* Raw GSL3680 sample recorder: streams every reported finger (including
 * samples the ID algorithm then drops) to /sdcard/panel/touch.csv. */
static void touch_raw_log_cb(uint32_t ms,
                             uint16_t raw_x, uint16_t raw_y, uint8_t raw_f,
                             uint16_t out_x, uint16_t out_y, uint8_t out_f)
{
    data_log_touch(ms, raw_x, raw_y, raw_f, out_x, out_y, out_f);
}

/* --- Touch debug overlay -------------------------------------------------
 * Draws a persistent red dot at every LVGL screen point the indev reports,
 * so a finger sweep leaves a visible breadcrumb trail.  Gaps in the trail
 * show exactly where the touch controller stops reporting (dead zones). */
#define TOUCH_DEBUG_DOT_COUNT 400

static lv_obj_t *s_touch_dots[TOUCH_DEBUG_DOT_COUNT];
static int s_touch_dot_next = 0;
static uint32_t s_touch_log_last_ms = 0;
static bool s_touch_debug_enabled = false;

static void touch_debug_overlay_create(void)
{
    if (s_touch_dots[0] != NULL) {
        return; /* already created */
    }

    lv_obj_t *top = lv_layer_top();
    if (top == NULL) {
        ESP_LOGW(TAG_TOUCH, "No top layer for touch debug overlay");
        return;
    }

    for (int i = 0; i < TOUCH_DEBUG_DOT_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(top);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xFF2020), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_touch_dots[i] = dot;
    }

    ESP_LOGI(TAG_TOUCH, "Touch debug overlay ready (%d dots)", TOUCH_DEBUG_DOT_COUNT);
}

static void touch_debug_place_dot(lv_point_t pt)
{
    if (s_touch_dots[0] == NULL) {
        return;
    }
    lv_obj_t *dot = s_touch_dots[s_touch_dot_next];
    s_touch_dot_next = (s_touch_dot_next + 1) % TOUCH_DEBUG_DOT_COUNT;
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(dot, pt.x - 2, pt.y - 2);
    lv_obj_move_foreground(dot);
}

void touch_debug_set_enabled(bool enabled)
{
    s_touch_debug_enabled = enabled;
    if (enabled) {
        if (s_touch_dots[0] == NULL && display_lock(1000)) {
            touch_debug_overlay_create();
            display_unlock();
        }
    } else if (s_touch_dots[0] != NULL && display_lock(1000)) {
        for (int i = 0; i < TOUCH_DEBUG_DOT_COUNT; i++) {
            lv_obj_add_flag(s_touch_dots[i], LV_OBJ_FLAG_HIDDEN);
        }
        s_touch_dot_next = 0;
        display_unlock();
    }
    ESP_LOGI(TAG_TOUCH, "Touch debug overlay %s", enabled ? "enabled" : "disabled");
}

static void touch_activity_event_cb(lv_event_t *event)
{
    /* For indev-list events the target is the indev, the param is the
     * pressed object. lv_event_get_indev() returns the param (the object)
     * for PRESSED, so use lv_event_get_target() to get the indev. */
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(event);
    lv_obj_t *pressed = (lv_obj_t *)lv_event_get_param(event);
    lv_point_t pt = { 0, 0 };
    if (indev != NULL) {
        lv_indev_get_point(indev, &pt);
    }

    if (s_touch_debug_enabled) {
        touch_debug_place_dot(pt);

        /* Throttled serial log (10 Hz) so a sweep can be followed live. */
        uint32_t now = lv_tick_get();
        if (s_touch_log_last_ms == 0 || (now - s_touch_log_last_ms) >= 100) {
            s_touch_log_last_ms = now;
            ESP_LOGI(TAG_TOUCH, "Touch x=%d y=%d (obj=%p)",
                (int)pt.x, (int)pt.y, (void *)pressed);
        }
    }
    display_note_activity();
}

static lv_display_t *touch_get_display(void)
{
#if LV_VERSION_MAJOR >= 9
    return lv_display_get_default();
#else
    return lv_disp_get_default();
#endif
}

static void touch_release_handle(void)
{
    if (s_touch_handle != NULL) {
        esp_lcd_touch_del(s_touch_handle);
        s_touch_handle = NULL;
    }
}

static esp_err_t jc_touch_i2c_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = JC_TOUCH_I2C_PORT,
        .sda_io_num = JC_TOUCH_I2C_SDA,
        .scl_io_num = JC_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Enable internal pullups (matches known-good HomeTiles/chipguy reference).
         * The GSL3680 touch I2C bus on this board relies on these; without them
         * reads become marginal and occasionally corrupt (e.g. raw sense = 17226). */
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
}

i2c_master_bus_handle_t jc8012_i2c_bus_get(void)
{
    /* The ES8311 codec shares I2C_NUM_1 with the GSL3680 touch controller,
     * so expose the bus handle for board_extras audio init. */
    return s_i2c_bus;
}

/* GSL3680 I2C address selector. The driver uses this (via config.driver_data)
 * to drive the INT/A0 pin low during reset, selecting I2C address 0x40.
 * Without it the driver skips address selection and logs
 * "Unable to initialize the I2C address". */
static const esp_lcd_touch_io_gsl3680_config_t s_gsl3680_io_cfg = {
    .dev_addr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS,
};

static esp_err_t jc_touch_new(esp_lcd_touch_handle_t *out_touch)
{
    ESP_RETURN_ON_ERROR(jc_touch_i2c_init(), TAG_TOUCH, "I2C bus init failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = JC_TOUCH_X_MAX,
        .y_max = JC_TOUCH_Y_MAX,
        .rst_gpio_num = JC_TOUCH_RST_GPIO,
        .int_gpio_num = JC_TOUCH_INT_GPIO,
        .driver_data = &s_gsl3680_io_cfg,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG();
    tp_io_config.scl_speed_hz = JC_TOUCH_I2C_CLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_config, &tp_io_handle),
        TAG_TOUCH, "New touch panel IO failed");

    return esp_lcd_touch_new_i2c_gsl3680(tp_io_handle, &tp_cfg, out_touch);
}

static void touch_apply_display_rotation_alignment(esp_lcd_touch_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    /* Driver reports native portrait ranges (x = short axis 800,
     * y = long axis 1280). LVGL rotates indev points 270 deg into landscape
     * itself, so no axis swap is required. On this panel's mounting the raw
     * touch axes are 180 deg inverted relative to the display, so mirror both
     * axes (verified: the previous mirror_y-only mapping left the touch
     * upside-down / diagonally flipped). */
    const bool swap_xy = false;
    const bool mirror_x = true;
    const bool mirror_y = false;

    esp_err_t err = esp_lcd_touch_set_swap_xy(handle, swap_xy);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TOUCH, "esp_lcd_touch_set_swap_xy failed: %s", esp_err_to_name(err));
    }
    err = esp_lcd_touch_set_mirror_x(handle, mirror_x);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TOUCH, "esp_lcd_touch_set_mirror_x failed: %s", esp_err_to_name(err));
    }
    err = esp_lcd_touch_set_mirror_y(handle, mirror_y);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_TOUCH, "esp_lcd_touch_set_mirror_y failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG_TOUCH, "Touch rotation aligned (swap=%d, mirror_x=%d, mirror_y=%d)",
        swap_xy ? 1 : 0, mirror_x ? 1 : 0, mirror_y ? 1 : 0);
}

/* The GSL3680 on this panel is polled in TIMER mode (10 ms), exactly like the
 * other Waveshare panels. EVENT mode (INT-driven, via the GSL3680 INT line on
 * GPIO21) was tried to reduce touch lag, but on this board the INT line does
 * NOT fire reliably: LVGL then never reads touch at all (the read timer stays
 * paused in EVENT mode), which presents as a completely dead touchscreen.
 * TIMER mode + 10 ms is the known-good configuration (see
 * ANALIZA-NAPRAWA-DOTYKU.md). */
static void touch_apply_polling_tune(lv_indev_t *indev)
{
    if (indev == NULL) {
        return;
    }

    lv_indev_set_mode(indev, LV_INDEV_MODE_TIMER);
    lv_timer_t *read_timer = lv_indev_get_read_timer(indev);
    if (read_timer != NULL) {
        lv_timer_set_period(read_timer, TOUCH_POLL_PERIOD_MS);
        lv_timer_ready(read_timer);
    }

    ESP_LOGI(TAG_TOUCH, "Touch polling tuned: mode=timer, period=%lu ms",
        (unsigned long)TOUCH_POLL_PERIOD_MS);
}

esp_err_t touch_init(void)
{
    if (s_touch_ready) {
        return ESP_OK;
    }
    if (!display_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    lv_display_t *disp = touch_get_display();
    if (disp == NULL) {
        ESP_LOGE(TAG_TOUCH, "No active LVGL display for touch binding");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= TOUCH_INIT_RETRIES; attempt++) {
        err = jc_touch_new(&s_touch_handle);
        if (err == ESP_OK && s_touch_handle != NULL) {
            break;
        }
        ESP_LOGW(TAG_TOUCH, "jc_touch_new attempt %d/%d failed: %s",
            attempt, TOUCH_INIT_RETRIES, esp_err_to_name(err));
        if (attempt < TOUCH_INIT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(TOUCH_INIT_RETRY_DELAY_MS));
        }
    }
    if (err != ESP_OK || s_touch_handle == NULL) {
        ESP_LOGE(TAG_TOUCH, "jc_touch_new failed after %d attempts: %s",
            TOUCH_INIT_RETRIES, esp_err_to_name(err));
        return (err == ESP_OK) ? ESP_FAIL : err;
    }

    touch_apply_display_rotation_alignment(s_touch_handle);

    /* Start persistent raw touch recording (SD card + serial). */
    esp_lcd_touch_gsl3680_set_raw_cb(touch_raw_log_cb);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = disp,
        .handle = s_touch_handle,
        .scale = {
            .x = 1.0f,
            .y = 1.0f,
        },
    };

    if (!display_lock(1000)) {
        touch_release_handle();
        return ESP_ERR_TIMEOUT;
    }
    s_touch_indev = lvgl_port_add_touch(&touch_cfg);
    if (s_touch_indev != NULL) {
        lv_indev_add_event_cb(s_touch_indev, touch_activity_event_cb, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(s_touch_indev, touch_activity_event_cb, LV_EVENT_PRESSING, NULL);
    }
    display_unlock();

    if (s_touch_indev == NULL) {
        ESP_LOGE(TAG_TOUCH, "lvgl_port_add_touch failed");
        touch_release_handle();
        return ESP_FAIL;
    }

    touch_apply_polling_tune(s_touch_indev);

    /* The debug overlay (when enabled) is created via touch_debug_set_enabled()
     * after display init, before touch_init runs. */

    s_touch_ready = true;
    ESP_LOGI(TAG_TOUCH, "Touch initialized (esp_lvgl_port + GSL3680)");
    return ESP_OK;
}

bool touch_is_ready(void)
{
    return s_touch_ready;
}
