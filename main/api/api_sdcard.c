/* SPDX-License-Identifier: LicenseRef-FNCL-1.1
 * Copyright (c) 2026 Cpt_Kirk
 *
 * SD-card management API (JC8012P4A1C 10.1" panel only):
 *   GET  /api/sd/status    -> mount/space/logging status
 *   POST /api/sd/format    -> explicit "set the card up as new" (FAT32 format)
 *   GET  /api/sd/bg/list   -> tile background images in /sdcard/bg
 *   POST /api/sd/bg/upload -> upload a tile background image
 */
#include "api/api_routes.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"

#include "app_config.h"
#include "diag/data_log.h"
#include "drivers/board_extras_panel10jc.h"
#include "util/log_tags.h"

#define TAG TAG_SD

#define SD_BG_DIR "/sdcard/bg"
#define SD_BG_NAME_MAX 64
#define SD_BG_LIST_MAX 64

static void set_json_headers(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", (message != NULL) ? message : "Invalid request");
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

static void add_space_to_object(cJSON *obj, const char *key_total, const char *key_free)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (sdcard_info(&total, &free_bytes) == ESP_OK) {
        cJSON_AddNumberToObject(obj, key_total, (double)total);
        cJSON_AddNumberToObject(obj, key_free, (double)free_bytes);
    } else {
        cJSON_AddNullToObject(obj, key_total);
        cJSON_AddNullToObject(obj, key_free);
    }
}

esp_err_t api_sd_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "mounted", sdcard_is_ready());
    cJSON_AddBoolToObject(root, "logging_enabled", data_log_is_enabled());
    cJSON_AddBoolToObject(root, "lost", data_log_is_lost());
    add_space_to_object(root, "total_bytes", "free_bytes");

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t api_sd_format_post_handler(httpd_req_t *req)
{
    /* Require an explicit {"confirm":true} so a stray request can never wipe
     * a card that has data on it. */
    if (req->content_len <= 0 || req->content_len > 256) {
        return send_json_error(req, "400 Bad Request", "Invalid payload size");
    }

    char *buf = calloc((size_t)req->content_len + 1U, sizeof(char));
    if (buf == NULL) {
        return httpd_resp_send_500(req);
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return send_json_error(req, "400 Bad Request", "Failed to read request body");
        }
        received += r;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        return send_json_error(req, "400 Bad Request", "Invalid JSON");
    }

    cJSON *confirm = cJSON_GetObjectItemCaseSensitive(root, "confirm");
    bool confirmed = cJSON_IsTrue(confirm);
    cJSON_Delete(root);
    if (!confirmed) {
        return send_json_error(req, "400 Bad Request", "Missing confirm:true");
    }

    ESP_LOGW(TAG, "Formatting SD card as new (user-initiated from web UI)");

    /* Stop the writer so no file handle is open while the card is unmounted. */
    data_log_suspend();

    esp_err_t fmt_err = sdcard_format_and_remount();
    if (fmt_err != ESP_OK) {
        (void)data_log_resume();
        return send_json_error(req, "500 Internal Server Error", "Format or re-mount failed");
    }

    esp_err_t resume_err = data_log_resume();
    if (resume_err != ESP_OK) {
        ESP_LOGW(TAG, "SD format OK but logging did not resume: %s", esp_err_to_name(resume_err));
    }

    cJSON *resp = cJSON_CreateObject();
    if (resp == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "mounted", sdcard_is_ready());
    add_space_to_object(resp, "total_bytes", "free_bytes");

    char *payload = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

/* Tile background images live in /sdcard/bg. Accept only safe, short names
 * ending in .png/.jpg/.jpeg so the browser can never write outside the dir. */
static bool sd_bg_name_ok(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    size_t n = strlen(name);
    if (n >= SD_BG_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            continue;
        }
        return false;
    }
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    if (strcasecmp(dot, ".png") == 0) {
        return true;
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return true;
    }
    return false;
}

esp_err_t api_sd_bg_list_get_handler(httpd_req_t *req)
{
    (void)mkdir(SD_BG_DIR, 0777);

    struct {
        char name[SD_BG_NAME_MAX];
        uint32_t size;
    } items[SD_BG_LIST_MAX];
    int count = 0;

    DIR *d = opendir(SD_BG_DIR);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL && count < SD_BG_LIST_MAX) {
            if (e->d_type == DT_DIR) {
                continue;
            }
            if (!sd_bg_name_ok(e->d_name)) {
                continue;
            }
            strlcpy(items[count].name, e->d_name, sizeof(items[count].name));

            char full[APP_MAX_IMAGE_PATH_LEN];
            snprintf(full, sizeof(full), "%s/%s", SD_BG_DIR, items[count].name);
            struct stat st;
            items[count].size = (stat(full, &st) == 0) ? (uint32_t)st.st_size : 0;
            count++;
        }
        closedir(d);
    }

    /* Keep the list deterministic for the editor dropdown. */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(items[i].name, items[j].name) > 0) {
                char tmp_name[SD_BG_NAME_MAX];
                strlcpy(tmp_name, items[i].name, sizeof(tmp_name));
                strlcpy(items[i].name, items[j].name, sizeof(items[i].name));
                strlcpy(items[j].name, tmp_name, sizeof(items[j].name));

                uint32_t tmp_size = items[i].size;
                items[i].size = items[j].size;
                items[j].size = tmp_size;
            }
        }
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    if (root == NULL || files == NULL) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        if (files != NULL) {
            cJSON_Delete(files);
        }
        return httpd_resp_send_500(req);
    }

    for (int i = 0; i < count; i++) {
        char full[APP_MAX_IMAGE_PATH_LEN];
        snprintf(full, sizeof(full), "%s/%s", SD_BG_DIR, items[i].name);

        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            continue;
        }
        cJSON_AddStringToObject(o, "name", items[i].name);
        cJSON_AddStringToObject(o, "path", full);
        cJSON_AddNumberToObject(o, "size", (double)items[i].size);
        cJSON_AddItemToArray(files, o);
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "files", files);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t api_sd_bg_delete_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char name[SD_BG_NAME_MAX + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "name", name, sizeof(name));
    }

    if (!sd_bg_name_ok(name)) {
        return send_json_error(req, "400 Bad Request", "name must be a short *.png/*.jpg filename");
    }

    char full[APP_MAX_IMAGE_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s", SD_BG_DIR, name);

    if (unlink(full) != 0) {
        return send_json_error(req, "404 Not Found", "File not found");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "path", full);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t api_sd_bg_upload_post_handler(httpd_req_t *req)
{
    char query[256] = {0};
    char name[SD_BG_NAME_MAX + 1] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "name", name, sizeof(name));
    }

    if (!sd_bg_name_ok(name)) {
        return send_json_error(req, "400 Bad Request", "name must be a short *.png/*.jpg filename");
    }

    (void)mkdir(SD_BG_DIR, 0777);

    char full[APP_MAX_IMAGE_PATH_LEN];
    snprintf(full, sizeof(full), "%s/%s", SD_BG_DIR, name);

    FILE *f = fopen(full, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "bg upload fopen failed: path=\"%s\" errno=%d (%s)", full, errno, strerror(errno));
        return send_json_error(req, "500 Internal Server Error", "Cannot open target file");
    }

    size_t total = 0;
    int remaining = req->content_len;
    uint8_t buf[512];
    while (remaining > 0) {
        int want = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r <= 0) {
            fclose(f);
            (void)unlink(full);
            return send_json_error(req, "400 Bad Request", "Failed to read upload body");
        }
        size_t written = fwrite(buf, 1, (size_t)r, f);
        if (written != (size_t)r) {
            fclose(f);
            (void)unlink(full);
            return send_json_error(req, "500 Internal Server Error", "Write failed");
        }
        total += written;
        remaining -= r;
    }
    fclose(f);
    ESP_LOGI(TAG, "bg upload ok: path=\"%s\" bytes=%u", full, (unsigned)total);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "path", full);
    cJSON_AddNumberToObject(root, "size", (double)total);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

#define SD_ROOT "/sdcard"
#define SD_LIST_MAX 256
#define SD_LIST_MAX_DEPTH 6
#define SD_FILE_PREVIEW_MAX (4U * 1024U * 1024U)

/* Resolve a client-supplied path against /sdcard, rejecting anything that
 * could escape the card (dot-dot segments, backslashes). */
static bool sd_resolve_path(const char *path, char *out, size_t out_len)
{
    if (path == NULL || path[0] == '\0' || out == NULL || out_len == 0) {
        return false;
    }
    if (strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return false;
    }

    const char *p = path;
    if (strncmp(p, SD_ROOT, sizeof(SD_ROOT) - 1) == 0) {
        p += sizeof(SD_ROOT) - 1;
    }
    while (*p == '/') {
        p++;
    }

    if (*p == '\0') {
        snprintf(out, out_len, "%s", SD_ROOT);
    } else {
        snprintf(out, out_len, "%s/%s", SD_ROOT, p);
    }
    return true;
}

/* Percent-decode a query value in place (decoded text is never longer than the
 * source, so it is safe to operate on the caller's buffer). Handles both %XX
 * and '+' (form-style space). */
static void url_decode_inplace(char *s)
{
    if (s == NULL) {
        return;
    }
    char *r = s;
    while (*s != '\0') {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            int hi = isdigit((unsigned char)s[1]) ? (s[1] - '0') : (tolower((unsigned char)s[1]) - 'a' + 10);
            int lo = isdigit((unsigned char)s[2]) ? (s[2] - '0') : (tolower((unsigned char)s[2]) - 'a' + 10);
            *r++ = (char)((hi << 4) | lo);
            s += 3;
        } else if (*s == '+') {
            *r++ = ' ';
            s++;
        } else {
            *r++ = *s++;
        }
    }
    *r = '\0';
}

static void sd_list_walk(cJSON *files, const char *dir_path, const char *rel, int depth, int *count)
{
    if (files == NULL || count == NULL || *count >= SD_LIST_MAX || depth > SD_LIST_MAX_DEPTH) {
        return;
    }

    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return;
    }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (*count >= SD_LIST_MAX) {
            break;
        }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }

        char full[512];
        char relpath[512];
        if (snprintf(full, sizeof(full), "%s/%s", dir_path, e->d_name) >= (int)sizeof(full)) {
            continue;
        }
        if (rel[0] == '\0') {
            if (snprintf(relpath, sizeof(relpath), "%s", e->d_name) >= (int)sizeof(relpath)) {
                continue;
            }
        } else {
            if (snprintf(relpath, sizeof(relpath), "%s/%s", rel, e->d_name) >= (int)sizeof(relpath)) {
                continue;
            }
        }

        struct stat st;
        bool is_dir = (e->d_type == DT_DIR);
        uint32_t size = 0;
        if (stat(full, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            size = (uint32_t)st.st_size;
        }

        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            continue;
        }
        cJSON_AddStringToObject(o, "name", e->d_name);
        cJSON_AddStringToObject(o, "path", full);
        cJSON_AddStringToObject(o, "rel", relpath);
        cJSON_AddNumberToObject(o, "size", (double)size);
        cJSON_AddBoolToObject(o, "is_dir", is_dir);
        cJSON_AddItemToArray(files, o);
        (*count)++;

        if (is_dir) {
            sd_list_walk(files, full, relpath, depth + 1, count);
        }
    }
    closedir(d);
}

esp_err_t api_sd_list_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    if (root == NULL || files == NULL) {
        if (root != NULL) {
            cJSON_Delete(root);
        }
        if (files != NULL) {
            cJSON_Delete(files);
        }
        return httpd_resp_send_500(req);
    }

    int count = 0;
    sd_list_walk(files, SD_ROOT, "", 0, &count);

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddItemToObject(root, "files", files);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        return httpd_resp_send_500(req);
    }

    set_json_headers(req);
    esp_err_t err = httpd_resp_sendstr(req, payload);
    cJSON_free(payload);
    return err;
}

esp_err_t api_sd_file_get_handler(httpd_req_t *req)
{
    char query[544] = {0};
    char path[513] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "path", path, sizeof(path));
    }
    url_decode_inplace(path);

    char full[512];
    if (!sd_resolve_path(path, full, sizeof(full))) {
        return send_json_error(req, "400 Bad Request", "Invalid path");
    }

    struct stat st;
    if (stat(full, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_json_error(req, "404 Not Found", "File not found");
    }
    if (st.st_size < 0 || (uint64_t)st.st_size > SD_FILE_PREVIEW_MAX) {
        return send_json_error(req, "413 Payload Too Large", "File too large to preview");
    }

    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        return send_json_error(req, "404 Not Found", "File not found");
    }

    const char *ct = "application/octet-stream";
    const char *dot = strrchr(full, '.');
    if (dot != NULL) {
        if (strcasecmp(dot, ".png") == 0) {
            ct = "image/png";
        } else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            ct = "image/jpeg";
        } else if (strcasecmp(dot, ".bmp") == 0) {
            ct = "image/bmp";
        } else if (strcasecmp(dot, ".gif") == 0) {
            ct = "image/gif";
        } else if (strcasecmp(dot, ".txt") == 0 || strcasecmp(dot, ".log") == 0) {
            ct = "text/plain";
        } else if (strcasecmp(dot, ".json") == 0) {
            ct = "application/json";
        }
    }

    httpd_resp_set_type(req, ct);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    uint8_t buf[1024];
    size_t remaining = (size_t)st.st_size;
    while (remaining > 0) {
        size_t want = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;
        }
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)got);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
        remaining -= got;
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}
