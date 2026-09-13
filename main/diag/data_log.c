/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * Resilient SD-card data logging (JC8012P4A1C 10.1" panel only).
 *
 * Design goals, in order of importance:
 *   1. A dead/removed/corrupt card must NEVER hang or crash the app.
 *   2. Producers (HA client, UI bindings, syslog capture, camera) must never
 *      block on filesystem I/O.
 *   3. A full queue must never grow without bound (records are dropped, not
 *      buffered forever), and sensor state must be rate-limited per entity.
 *   4. The card must survive years of continuous logging, so we:
 *      - keep the log files open and write many records per file operation,
 *      - buffer records in RAM and only fflush() when 16 KB has accumulated
 *        or 30 s has passed (fflush -> f_sync is the wear amplifier),
 *      - rotate small fixed-size files (256 KB system log, 1 MB sensor CSV)
 *        instead of growing one giant file.
 *
 * Everything is achieved with a single low-priority writer task fed by a
 * bounded queue.  Producers only ever copy small structs into a queue (or
 * hand off a JPEG blob); the writer task is the only place that touches the
 * filesystem.  When file I/O fails CONSEC_ERR_THRESHOLD times in a row the
 * module enters the "lost" state: it closes the files, stops touching the
 * card and retries a cheap probe every RETRY_COOLDOWN_MS until the card is
 * usable again.
 */
#include "diag/data_log.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "drivers/board_extras_panel10jc.h"
#include "util/log_tags.h"

#define TAG TAG_SD

/* ------------------------------------------------------------------ */
/* Tuning knobs                                                        */
/* ------------------------------------------------------------------ */
#define DATA_LOG_QUEUE_DEPTH         64
#define DATA_LOG_TASK_STACK          4096
#define DATA_LOG_TASK_PRIO           1
#define DATA_LOG_TASK_PERIOD_MS      1000
#define DATA_LOG_TEXT_MAX            600
#define DATA_LOG_SUSPEND_TIMEOUT_MS  15000

/* Consecutive failed file operations before entering the "lost" state. */
#define DATA_LOG_CONSEC_ERR_THRESHOLD 3
#define DATA_LOG_RETRY_COOLDOWN_MS    (30 * 1000)

/* RAM buffering: flush to the card only when this many bytes are pending
 * (or FLUSH_INTERVAL has elapsed), so a busy sensor stream does not
 * translate into a f_sync() per record. */
#define DATA_LOG_FLUSH_BYTES         (16 * 1024)
#define DATA_LOG_FLUSH_INTERVAL_MS   30000

/* Rotation: keep <base>, <base>.1, <base>.2, <base>.3. */
#define DATA_LOG_MAX_GENS            3
#define DATA_LOG_SYSTEM_LOG_MAX      (256u * 1024u)
#define DATA_LOG_SENSORS_MAX         (1024u * 1024u)
#define DATA_LOG_TOUCH_MAX           (1024u * 1024u)
#define DATA_LOG_CAMERA_MAX_FILES    50

/* Per-entity sensor rate limit. */
#define DATA_LOG_SENSOR_RATE_MS      5000
#define DATA_LOG_SENSOR_RATE_SLOTS   64

#define SD_PANEL_DIR                 "/sdcard/panel"
#define SD_SYSTEM_LOG_PATH           "/sdcard/panel/system.log"
#define SD_SENSORS_PATH              "/sdcard/panel/sensors.csv"
#define SD_TOUCH_PATH                "/sdcard/panel/touch.csv"
#define SD_CAMERA_DIR                "/sdcard/panel/camera"

/* ------------------------------------------------------------------ */
/* Record types                                                        */
/* ------------------------------------------------------------------ */
typedef enum {
    DATA_LOG_REC_MIRROR = 0,   /* text[] = raw syslog line */
    DATA_LOG_REC_SENSOR,       /* text[] = "entity_id|state" */
    DATA_LOG_REC_TOUCH,        /* text[] = "ms,raw_x,raw_y,raw_f,out_x,out_y,out_f" */
    DATA_LOG_REC_JPEG,         /* blob = JPEG bytes, text unused */
} data_log_rec_type_t;

typedef struct {
    uint8_t type;
    size_t len;
    char text[DATA_LOG_TEXT_MAX];
    uint8_t *blob;             /* owned by the record; freed by the writer */
} data_log_rec_t;

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static QueueHandle_t s_q = NULL;
static TaskHandle_t s_task = NULL;
static SemaphoreHandle_t s_task_done = NULL;

static volatile bool s_enabled = true;
static volatile bool s_log_system_enabled = true;
static volatile bool s_log_sensors_enabled = true;
static volatile bool s_log_camera_enabled = true;
static volatile int s_flush_interval_ms = DATA_LOG_FLUSH_INTERVAL_MS;
static volatile bool s_lost = false;
static volatile bool s_stop_requested = false;
static int s_consec_err = 0;
static int64_t s_last_retry_us = 0;
static int64_t s_last_flush_us = 0;

/* Open log files (kept open so a record does not cause open+sync+close). */
static FILE *s_sys_fp = NULL;
static FILE *s_csv_fp = NULL;
static FILE *s_touch_fp = NULL;
static size_t s_sys_written = 0;   /* bytes written to the current file */
static size_t s_sys_pending = 0;   /* bytes not yet flushed to the card */
static size_t s_csv_written = 0;
static size_t s_csv_pending = 0;
static size_t s_touch_written = 0;
static size_t s_touch_pending = 0;

static char s_sensor_id[DATA_LOG_SENSOR_RATE_SLOTS][APP_MAX_ENTITY_ID_LEN];
static int64_t s_sensor_last_ms[DATA_LOG_SENSOR_RATE_SLOTS];

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */
static void data_log_rec_free(data_log_rec_t *rec)
{
    if (rec->blob != NULL) {
        free(rec->blob);
        rec->blob = NULL;
    }
    rec->len = 0;
}

static bool data_log_enqueue(data_log_rec_t *rec)
{
    if (s_q == NULL || !s_enabled || s_stop_requested) {
        data_log_rec_free(rec);
        return false;
    }
    /* Never block: if the queue is full we drop the NEWEST record. */
    if (xQueueSendToBack(s_q, rec, 0) != pdTRUE) {
        data_log_rec_free(rec);
        return false;
    }
    return true;
}

static void data_log_ensure_dirs(void)
{
    mkdir(SD_PANEL_DIR, 0777);
    mkdir(SD_CAMERA_DIR, 0777);
}

static void data_log_rotate(const char *path)
{
    char old_path[128];
    char new_path[128];

    snprintf(old_path, sizeof(old_path), "%s.%d", path, DATA_LOG_MAX_GENS);
    remove(old_path);

    for (int i = DATA_LOG_MAX_GENS - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", path, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", path, i + 1);
        rename(old_path, new_path);
    }

    snprintf(old_path, sizeof(old_path), "%s.1", path);
    rename(path, old_path);
}

static size_t data_log_existing_size(FILE *f)
{
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        return 0;
    }
    long sz = ftell(f);
    return sz > 0 ? (size_t)sz : 0;
}

static bool data_log_open_sys(void)
{
    if (s_sys_fp != NULL) {
        return true;
    }
    s_sys_fp = fopen(SD_SYSTEM_LOG_PATH, "ab");
    if (s_sys_fp == NULL) {
        return false;
    }
    s_sys_written = data_log_existing_size(s_sys_fp);
    s_sys_pending = 0;
    return true;
}

static bool data_log_open_csv(void)
{
    if (s_csv_fp != NULL) {
        return true;
    }
    s_csv_fp = fopen(SD_SENSORS_PATH, "ab");
    if (s_csv_fp == NULL) {
        return false;
    }
    s_csv_written = data_log_existing_size(s_csv_fp);
    s_csv_pending = 0;
    return true;
}

static bool data_log_open_touch(void)
{
    if (s_touch_fp != NULL) {
        return true;
    }
    s_touch_fp = fopen(SD_TOUCH_PATH, "ab");
    if (s_touch_fp == NULL) {
        return false;
    }
    s_touch_written = data_log_existing_size(s_touch_fp);
    s_touch_pending = 0;
    return true;
}

static bool data_log_open_all(void)
{
    data_log_ensure_dirs();
    bool ok_sys = data_log_open_sys();
    bool ok_csv = data_log_open_csv();
    bool ok_touch = data_log_open_touch();
    s_last_flush_us = esp_timer_get_time();
    /* Any one file is enough for logging to proceed. */
    return ok_sys || ok_csv || ok_touch;
}

static void data_log_close_all(void)
{
    if (s_sys_fp != NULL) {
        (void)fflush(s_sys_fp);
        fclose(s_sys_fp);
        s_sys_fp = NULL;
    }
    if (s_csv_fp != NULL) {
        (void)fflush(s_csv_fp);
        fclose(s_csv_fp);
        s_csv_fp = NULL;
    }
    if (s_touch_fp != NULL) {
        (void)fflush(s_touch_fp);
        fclose(s_touch_fp);
        s_touch_fp = NULL;
    }
    s_sys_written = 0;
    s_sys_pending = 0;
    s_csv_written = 0;
    s_csv_pending = 0;
    s_touch_written = 0;
    s_touch_pending = 0;
}

/* File-op accounting: success resets the error state, failure counts up and
 * eventually trips the "lost" latch. */
static void data_log_note_success(void)
{
    if (s_consec_err != 0 || s_lost) {
        s_consec_err = 0;
        s_lost = false;
    }
}

static void data_log_note_error(void)
{
    s_consec_err++;
    if (s_consec_err >= DATA_LOG_CONSEC_ERR_THRESHOLD && !s_lost) {
        s_lost = true;
        data_log_close_all();
        ESP_LOGW(TAG, "SD logging disabled after %d consecutive I/O errors; "
                 "retrying every %d s", s_consec_err,
                 DATA_LOG_RETRY_COOLDOWN_MS / 1000);
    }
}

static bool data_log_sensor_rate_limited(const char *entity_id, size_t entity_len)
{
    int64_t now = (int64_t)(esp_timer_get_time() / 1000);

    for (int i = 0; i < DATA_LOG_SENSOR_RATE_SLOTS; i++) {
        if (s_sensor_last_ms[i] != 0 &&
            strncmp(s_sensor_id[i], entity_id, sizeof(s_sensor_id[i]) - 1) == 0 &&
            strlen(s_sensor_id[i]) == entity_len) {
            if (now - s_sensor_last_ms[i] < DATA_LOG_SENSOR_RATE_MS) {
                return true; /* too soon */
            }
            s_sensor_last_ms[i] = now;
            return false;
        }
    }

    /* First time we see this entity: claim an empty slot. */
    for (int i = 0; i < DATA_LOG_SENSOR_RATE_SLOTS; i++) {
        if (s_sensor_last_ms[i] == 0) {
            strncpy(s_sensor_id[i], entity_id, sizeof(s_sensor_id[i]) - 1);
            s_sensor_id[i][sizeof(s_sensor_id[i]) - 1] = '\0';
            s_sensor_last_ms[i] = now;
            return false;
        }
    }
    return false; /* table full: write it (best effort) */
}

/* ------------------------------------------------------------------ */
/* Buffered file writers (writer task only)                            */
/* ------------------------------------------------------------------ */
static bool data_log_rotate_sys(void)
{
    if (s_sys_fp != NULL) {
        (void)fflush(s_sys_fp);
        fclose(s_sys_fp);
        s_sys_fp = NULL;
    }
    data_log_rotate(SD_SYSTEM_LOG_PATH);
    s_sys_written = 0;
    s_sys_pending = 0;
    return data_log_open_sys();
}

static bool data_log_rotate_csv(void)
{
    if (s_csv_fp != NULL) {
        (void)fflush(s_csv_fp);
        fclose(s_csv_fp);
        s_csv_fp = NULL;
    }
    data_log_rotate(SD_SENSORS_PATH);
    s_csv_written = 0;
    s_csv_pending = 0;
    return data_log_open_csv();
}

static bool data_log_rotate_touch(void)
{
    if (s_touch_fp != NULL) {
        (void)fflush(s_touch_fp);
        fclose(s_touch_fp);
        s_touch_fp = NULL;
    }
    data_log_rotate(SD_TOUCH_PATH);
    s_touch_written = 0;
    s_touch_pending = 0;
    return data_log_open_touch();
}

static void data_log_write_mirror(const char *data, size_t len)
{
    if (!sdcard_is_ready()) {
        data_log_note_error();
        return;
    }
    if ((s_sys_fp == NULL && !data_log_open_sys())) {
        data_log_note_error();
        return;
    }
    if (s_sys_written + len > DATA_LOG_SYSTEM_LOG_MAX) {
        if (!data_log_rotate_sys()) {
            data_log_note_error();
            return;
        }
    }

    size_t w = fwrite(data, 1, len, s_sys_fp);
    if (w != len) {
        data_log_note_error();
        return;
    }
    s_sys_written += w;
    s_sys_pending += w;
    if (s_sys_pending >= DATA_LOG_FLUSH_BYTES) {
        if (fflush(s_sys_fp) != 0) {
            data_log_note_error();
            return;
        }
        s_sys_pending = 0;
    }
    data_log_note_success();
}

static void data_log_write_sensor_line(const char *line, size_t len)
{
    if (!sdcard_is_ready()) {
        data_log_note_error();
        return;
    }
    if ((s_csv_fp == NULL && !data_log_open_csv())) {
        data_log_note_error();
        return;
    }
    if (s_csv_written + len > DATA_LOG_SENSORS_MAX) {
        if (!data_log_rotate_csv()) {
            data_log_note_error();
            return;
        }
    }

    size_t w = fwrite(line, 1, len, s_csv_fp);
    if (w != len) {
        data_log_note_error();
        return;
    }
    s_csv_written += w;
    s_csv_pending += w;
    if (s_csv_pending >= DATA_LOG_FLUSH_BYTES) {
        if (fflush(s_csv_fp) != 0) {
            data_log_note_error();
            return;
        }
        s_csv_pending = 0;
    }
    data_log_note_success();
}

static void data_log_write_touch_line(const char *line, size_t len)
{
    if (!sdcard_is_ready()) {
        data_log_note_error();
        return;
    }
    if ((s_touch_fp == NULL && !data_log_open_touch())) {
        data_log_note_error();
        return;
    }
    if (s_touch_written + len > DATA_LOG_TOUCH_MAX) {
        if (!data_log_rotate_touch()) {
            data_log_note_error();
            return;
        }
    }

    size_t w = fwrite(line, 1, len, s_touch_fp);
    if (w != len) {
        data_log_note_error();
        return;
    }
    s_touch_written += w;
    s_touch_pending += w;
    if (s_touch_pending >= DATA_LOG_FLUSH_BYTES) {
        if (fflush(s_touch_fp) != 0) {
            data_log_note_error();
            return;
        }
        s_touch_pending = 0;
    }
    data_log_note_success();
}

static void data_log_periodic_flush(void)
{
    int64_t now_us = esp_timer_get_time();
    int interval_ms = s_flush_interval_ms > 0 ? s_flush_interval_ms : DATA_LOG_FLUSH_INTERVAL_MS;
    if (now_us - s_last_flush_us < interval_ms * 1000LL) {
        return;
    }
    s_last_flush_us = now_us;

    if (s_sys_fp != NULL && s_sys_pending > 0) {
        if (fflush(s_sys_fp) != 0) {
            data_log_note_error();
            return;
        }
        s_sys_pending = 0;
    }
    if (s_csv_fp != NULL && s_csv_pending > 0) {
        if (fflush(s_csv_fp) != 0) {
            data_log_note_error();
            return;
        }
        s_csv_pending = 0;
    }
    if (s_touch_fp != NULL && s_touch_pending > 0) {
        if (fflush(s_touch_fp) != 0) {
            data_log_note_error();
            return;
        }
        s_touch_pending = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Record processing (writer task only)                                */
/* ------------------------------------------------------------------ */
static void data_log_prune_camera(void);

static void data_log_process(const data_log_rec_t *rec)
{
    switch (rec->type) {
    case DATA_LOG_REC_MIRROR:
        data_log_write_mirror(rec->text, rec->len);
        break;

    case DATA_LOG_REC_SENSOR: {
        const char *pipe = memchr(rec->text, '|', rec->len);
        size_t entity_len = pipe ? (size_t)(pipe - rec->text) : rec->len;
        const char *state = pipe ? pipe + 1 : "";

        /* Rate-limit per entity: a chatty power meter must not flood the
         * CSV with hundreds of lines per second. */
        char entity_buf[APP_MAX_ENTITY_ID_LEN];
        size_t copy_len = entity_len < sizeof(entity_buf) - 1
                              ? entity_len
                              : sizeof(entity_buf) - 1;
        memcpy(entity_buf, rec->text, copy_len);
        entity_buf[copy_len] = '\0';

        if (data_log_sensor_rate_limited(entity_buf, entity_len)) {
            break;
        }

        char line[DATA_LOG_TEXT_MAX + 64];
        time_t now = time(NULL);
        int n = snprintf(line, sizeof(line), "%lld,%.*s,%s\n",
                         (long long)now, (int)entity_len, rec->text, state);
        if (n < 0) {
            break;
        }
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        data_log_write_sensor_line(line, len);
        break;
    }

    case DATA_LOG_REC_TOUCH: {
        /* No rate limit: a touch recorder must capture every sample or the
         * dead-band trace becomes useless. */
        char line[DATA_LOG_TEXT_MAX + 64];
        int n = snprintf(line, sizeof(line), "%s\n", rec->text);
        if (n < 0) {
            break;
        }
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        data_log_write_touch_line(line, len);
        break;
    }

    case DATA_LOG_REC_JPEG: {
        if (!sdcard_is_ready()) {
            data_log_note_error();
            break;
        }
        data_log_ensure_dirs();

        char path[160];
        int64_t ms = esp_timer_get_time() / 1000;
        snprintf(path, sizeof(path), "%s/cam_%lld.jpg", SD_CAMERA_DIR,
                 (long long)ms);

        FILE *f = fopen(path, "wb");
        if (f == NULL) {
            data_log_note_error();
            break;
        }
        bool ok = fwrite(rec->blob, 1, rec->len, f) == rec->len;
        bool closed = fclose(f) == 0;

        if (ok && closed) {
            data_log_note_success();
            data_log_prune_camera();
        } else {
            remove(path);
            data_log_note_error();
        }
        break;
    }

    default:
        break;
    }
}

/* Keep at most DATA_LOG_CAMERA_MAX_FILES images; filenames are time-sorted
 * (cam_<ms>.jpg) so the lexicographically smallest name is the oldest. */
static void data_log_prune_camera(void)
{
    int count = 0;
    DIR *d = opendir(SD_CAMERA_DIR);
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "cam_", 4) == 0) {
            count++;
        }
    }
    closedir(d);

    while (count > DATA_LOG_CAMERA_MAX_FILES) {
        char oldest[64] = {0};
        bool found = false;

        d = opendir(SD_CAMERA_DIR);
        if (d == NULL) {
            return;
        }
        while ((e = readdir(d)) != NULL) {
            if (strncmp(e->d_name, "cam_", 4) != 0) {
                continue;
            }
            if (!found || strcmp(e->d_name, oldest) < 0) {
                strlcpy(oldest, e->d_name, sizeof(oldest));
                found = true;
            }
        }
        closedir(d);

        if (!found) {
            break;
        }

        char full[160];
        snprintf(full, sizeof(full), "%s/%s", SD_CAMERA_DIR, oldest);
        if (remove(full) != 0) {
            break;
        }
        count--;
    }
}

/* ------------------------------------------------------------------ */
/* Writer task                                                         */
/* ------------------------------------------------------------------ */
static void data_log_writer_task(void *arg)
{
    (void)arg;

    for (;;) {
        if (s_stop_requested) {
            break;
        }

        /* "Lost" latch: probe periodically instead of hammering a dead
         * card on every record. */
        if (s_lost) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - s_last_retry_us >= DATA_LOG_RETRY_COOLDOWN_MS * 1000LL) {
                s_last_retry_us = now_us;
                if (sdcard_is_ready() && data_log_open_all()) {
                    s_lost = false;
                    s_consec_err = 0;
                    ESP_LOGI(TAG, "SD logging recovered");
                }
            }
        }

        data_log_rec_t rec;
        while (xQueueReceive(s_q, &rec, 0) == pdTRUE) {
            if (s_stop_requested) {
                data_log_rec_free(&rec);
                break;
            }
            if (!s_enabled) {
                data_log_rec_free(&rec);
                continue;
            }
            if (s_lost) {
                data_log_rec_free(&rec);
                continue;
            }
            data_log_process(&rec);
            data_log_rec_free(&rec);
        }

        data_log_periodic_flush();

        /* Logging turned off: flush and close so the user can pull the card
         * without losing the buffered tail and without an open file handle. */
        if (!s_enabled) {
            data_log_close_all();
        }

        if (s_stop_requested) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DATA_LOG_TASK_PERIOD_MS));
    }

    data_log_close_all();
    ESP_LOGI(TAG, "data_log writer task stopped");
    xSemaphoreGive(s_task_done);
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */
esp_err_t data_log_init(void)
{
    if (s_q != NULL) {
        return ESP_OK;
    }

    if (!sdcard_is_ready()) {
        ESP_LOGW(TAG, "SD not mounted; data logging disabled until a card is present");
        return ESP_ERR_INVALID_STATE;
    }

    s_q = xQueueCreate(DATA_LOG_QUEUE_DEPTH, sizeof(data_log_rec_t));
    if (s_q == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_task_done = xSemaphoreCreateBinary();
    if (s_task_done == NULL) {
        vQueueDelete(s_q);
        s_q = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_enabled = true;
    s_lost = false;
    s_stop_requested = false;
    s_consec_err = 0;
    s_last_retry_us = 0;
    s_last_flush_us = 0;
    memset(s_sensor_id, 0, sizeof(s_sensor_id));
    memset(s_sensor_last_ms, 0, sizeof(s_sensor_last_ms));

    data_log_ensure_dirs();

    BaseType_t ok = xTaskCreate(data_log_writer_task, "data_log",
                                DATA_LOG_TASK_STACK, NULL,
                                DATA_LOG_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_task_done);
        s_task_done = NULL;
        vQueueDelete(s_q);
        s_q = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* Boot marker so the SD log always has a clear start boundary.  It is
     * enqueued (not written directly) so all file I/O stays on the writer
     * task. */
    const char marker[] = "=== SD logging started ===\n";
    data_log_rec_t rec = {0};
    rec.type = DATA_LOG_REC_MIRROR;
    rec.len = sizeof(marker) - 1;
    memcpy(rec.text, marker, rec.len);
    rec.text[rec.len] = '\0';
    (void)xQueueSendToBack(s_q, &rec, 0);

    ESP_LOGI(TAG, "SD data logging enabled (system.log, sensors.csv, touch.csv, camera)");
    return ESP_OK;
}

void data_log_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        if (s_task != NULL) {
            /* Wake the writer so it flushes and closes the files promptly.
             * The close happens on the writer task, never here. */
            xTaskNotifyGive(s_task);
        } else {
            /* Writer task not running yet (boot-time call). */
            data_log_close_all();
        }
    }
}

void data_log_set_flush_interval_s(int seconds)
{
    if (seconds < 5) {
        seconds = 5;
    }
    if (seconds > 300) {
        seconds = 300;
    }
    s_flush_interval_ms = seconds * 1000;
    /* Reset the last-flush timestamp so a shortened interval takes effect
     * immediately instead of waiting for the previous long interval. */
    s_last_flush_us = 0;
}

void data_log_set_log_system_enabled(bool enabled)
{
    s_log_system_enabled = enabled;
}

void data_log_set_log_sensors_enabled(bool enabled)
{
    s_log_sensors_enabled = enabled;
}

void data_log_set_log_camera_enabled(bool enabled)
{
    s_log_camera_enabled = enabled;
}

bool data_log_is_enabled(void)
{
    return s_enabled;
}

bool data_log_is_lost(void)
{
    return s_lost;
}

void data_log_mirror_syslog(const char *line, size_t len)
{
    if (line == NULL || len == 0) {
        return;
    }
    if (!s_log_system_enabled) {
        return;
    }
    if (len > DATA_LOG_TEXT_MAX - 1) {
        len = DATA_LOG_TEXT_MAX - 1;
    }

    data_log_rec_t rec = {0};
    rec.type = DATA_LOG_REC_MIRROR;
    rec.len = len;
    memcpy(rec.text, line, len);
    rec.text[len] = '\0';
    data_log_enqueue(&rec);
}

void data_log_sensor(const char *entity_id, const char *state)
{
    if (entity_id == NULL || state == NULL) {
        return;
    }
    if (!s_log_sensors_enabled) {
        return;
    }

    /* entity_id uses "domain.name" style with no '|' or control chars, so
     * '|' is a safe separator.  Sanitize anything hostile in the state. */
    char buf[DATA_LOG_TEXT_MAX];
    int n = snprintf(buf, sizeof(buf), "%s|%s", entity_id, state);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == ',' || c == '\n' || c == '\r' || c == '\t') {
            buf[i] = '_';
        }
    }
    buf[len] = '\0';

    data_log_rec_t rec = {0};
    rec.type = DATA_LOG_REC_SENSOR;
    rec.len = len;
    memcpy(rec.text, buf, len + 1);
    data_log_enqueue(&rec);
}

void data_log_touch(uint32_t ms,
                    uint16_t raw_x, uint16_t raw_y, uint8_t raw_f,
                    uint16_t out_x, uint16_t out_y, uint8_t out_f)
{
    char buf[DATA_LOG_TEXT_MAX];
    int n = snprintf(buf, sizeof(buf),
                     "%lu,%u,%u,%u,%u,%u,%u",
                     (unsigned long)ms,
                     (unsigned)raw_x, (unsigned)raw_y, (unsigned)raw_f,
                     (unsigned)out_x, (unsigned)out_y, (unsigned)out_f);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n;
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }

    data_log_rec_t rec = {0};
    rec.type = DATA_LOG_REC_TOUCH;
    rec.len = len;
    memcpy(rec.text, buf, len);
    rec.text[len] = '\0';
    data_log_enqueue(&rec);
}

void data_log_save_jpeg(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (!s_log_camera_enabled) {
        return;
    }

    /* JPEG frames can be ~200 KB; prefer PSRAM so the internal heap is not
     * fragmented by a burst of camera captures. */
    uint8_t *blob = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (blob == NULL) {
        blob = malloc(len);
    }
    if (blob == NULL) {
        ESP_LOGW(TAG, "jpeg save: out of memory (%u bytes)", (unsigned)len);
        return;
    }
    memcpy(blob, data, len);

    data_log_rec_t rec = {0};
    rec.type = DATA_LOG_REC_JPEG;
    rec.len = len;
    rec.blob = blob;
    data_log_enqueue(&rec);
}

void data_log_suspend(void)
{
    if (s_q == NULL) {
        return;
    }

    s_stop_requested = true;

    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
        if (xSemaphoreTake(s_task_done, pdMS_TO_TICKS(DATA_LOG_SUSPEND_TIMEOUT_MS)) == pdTRUE) {
            s_task = NULL; /* confirmed exited */
        } else {
            /* The SDMMC driver has a bounded command timeout, so this should
             * never happen in practice; proceed rather than hang the format
             * flow forever. */
            ESP_LOGW(TAG, "data_log writer did not stop within %d ms",
                     DATA_LOG_SUSPEND_TIMEOUT_MS);
        }
    }
}

esp_err_t data_log_resume(void)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = false;
    s_lost = false;
    s_consec_err = 0;
    s_last_retry_us = 0;
    s_last_flush_us = 0;

    data_log_ensure_dirs();

    if (s_task == NULL && sdcard_is_ready()) {
        BaseType_t ok = xTaskCreate(data_log_writer_task, "data_log",
                                    DATA_LOG_TASK_STACK, NULL,
                                    DATA_LOG_TASK_PRIO, &s_task);
        if (ok != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}
