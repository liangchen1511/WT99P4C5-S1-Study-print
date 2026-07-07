/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_print_sync.h"
#include "parent_print_fs.h"

#include "soti_config.h"
#include "escpos_text_print.h"
#include "parent_album_sync.h"
#include "parent_policy.hpp"
#include "parent_net_gate.h"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "power_manager.h"

extern "C" void __attribute__((weak)) parent_chat_bg_pause(bool paused)
{
    (void)paused;
}

static const char *TAG = "prt_sync";

static parent_print_sync_status_fn_t s_status_fn;
static void *s_status_user;
static uint32_t s_pending;
static std::vector<parent_print_job_t> s_jobs;
static TaskHandle_t s_bg_task;
static volatile bool s_bg_run;
static volatile bool s_paused;
static volatile bool s_busy;
static SemaphoreHandle_t s_job_mux;
static bool s_sdio_bg_paused;

static bool have_sta_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

static void sdio_bg_http_pause(bool pause)
{
    if (pause) {
        if (s_sdio_bg_paused) {
            return;
        }
        s_sdio_bg_paused = true;
        parent_album_sync_pause(true);
        parent_print_sync_pause(true);
        parent_chat_bg_pause(true);
        parent_policy_poll_pause(true);
        ESP_LOGD(TAG, "SDIO bg HTTP paused (print)");
        return;
    }
    if (!s_sdio_bg_paused) {
        return;
    }
    s_sdio_bg_paused = false;
    if (!power_manager_is_screen_on() || !have_sta_ip()) {
        ESP_LOGD(TAG, "SDIO bg HTTP stay paused (no ip or screen off)");
        return;
    }
    parent_album_sync_pause(false);
    parent_print_sync_pause(false);
    parent_chat_bg_pause(false);
    parent_policy_poll_pause(false);
    ESP_LOGD(TAG, "SDIO bg HTTP resumed (print done)");
}

static void notify_status(const char *line)
{
    if (s_status_fn != nullptr && line != nullptr) {
        s_status_fn(line, s_status_user);
    }
}

static bool build_api_url(char *dst, size_t cap, const char *path)
{
    const char *base = SOTI_R2_WORKER_URL;
    if (base == nullptr || path == nullptr || cap < 64) {
        return false;
    }
    const char *suffix = "/upload";
    size_t lu = strlen(base);
    size_t ls = strlen(suffix);
    if (lu >= ls && strcmp(base + lu - ls, suffix) == 0) {
        size_t prefix = lu - ls;
        int n = snprintf(dst, cap, "%.*s%s", (int)prefix, base, path);
        return n > 0 && (size_t)n < cap;
    }
    int n = snprintf(dst, cap, "%s%s", base, path);
    return n > 0 && (size_t)n < cap;
}

static void http_tls_if_https(esp_http_client_config_t *cfg, const char *url)
{
    if (cfg == nullptr || url == nullptr || strncmp(url, "https://", 8) != 0) {
        return;
    }
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
    cfg->tls_version = ESP_HTTP_CLIENT_TLS_VER_TLS_1_2;
}

static void apply_bearer(esp_http_client_handle_t client)
{
    if (SOTI_R2_UPLOAD_TOKEN[0] != '\0') {
        char auth[192];
        int n = snprintf(auth, sizeof(auth), "Bearer %s", SOTI_R2_UPLOAD_TOKEN);
        if (n > 0 && (size_t)n < sizeof(auth)) {
            esp_http_client_set_header(client, "Authorization", auth);
        }
    }
    esp_http_client_set_header(client, "X-Device-Id", SOTI_DEVICE_ID);
}

static bool http_get_text(const char *path_query, std::string &out, int *status_out)
{
    if (!parent_net_http_allowed()) {
        return false;
    }
    char url[360];
    if (!build_api_url(url, sizeof(url), path_query)) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 20000;
    http_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    apply_bearer(client);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    out.clear();
    out.resize(8192);
    int total = 0;
    while (total < (int)out.size() - 1) {
        int r = esp_http_client_read(client, &out[total], (int)out.size() - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    out[total] = '\0';
    out.resize(total);
    int st = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (status_out) {
        *status_out = st;
    }
    return st == 200;
}

static bool http_post_json(const char *path, const char *json, std::string &out)
{
    if (!parent_net_http_allowed()) {
        return false;
    }
    char url[360];
    if (!build_api_url(url, sizeof(url), path)) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 15000;
    http_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    apply_bearer(client);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    const char *body = json ? json : "{}";
    int blen = (int)strlen(body);
    if (esp_http_client_open(client, blen) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    if (blen > 0) {
        (void)esp_http_client_write(client, body, blen);
    }
    (void)esp_http_client_fetch_headers(client);
    out.clear();
    out.resize(512);
    int total = 0;
    while (total < (int)out.size() - 1) {
        int r = esp_http_client_read(client, &out[total], (int)out.size() - 1 - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    out[total] = '\0';
    int st = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return st == 200;
}

static bool http_get_binary_chunk(const char *path_query, size_t len, uint8_t *buf, size_t *got)
{
    *got = 0;
    if (!parent_net_http_allowed()) {
        return false;
    }
    char url[400];
    if (!build_api_url(url, sizeof(url), path_query)) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 30000;
    http_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    apply_bearer(client);
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    int st = esp_http_client_get_status_code(client);
    if (st != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    int total = 0;
    int idle = 0;
    while (total < (int)len) {
        int chunk = (int)len - total;
        if (chunk > 512) {
            chunk = 512;
        }
        int r = esp_http_client_read(client, (char *)buf + total, chunk);
        if (r == 0) {
            if (++idle > 200) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        idle = 0;
        if (r < 0) {
            break;
        }
        total += r;
        if (r >= 256) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    *got = (size_t)total;
    return total > 0;
}

static wifi_ps_type_t s_saved_ps;
static bool s_saved_ps_valid;
static bool s_focus_on;

static void focus_enter(void)
{
    if (s_focus_on) {
        return;
    }
    s_focus_on = true;
    sdio_bg_http_pause(true);
    s_saved_ps_valid = (esp_wifi_get_ps(&s_saved_ps) == ESP_OK);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    power_manager_block_sleep("prt_sync");
}

static void focus_exit(void)
{
    if (!s_focus_on) {
        return;
    }
    power_manager_unblock_sleep("prt_sync");
    if (s_saved_ps_valid) {
        (void)esp_wifi_set_ps(s_saved_ps);
    }
    sdio_bg_http_pause(false);
    s_focus_on = false;
}

void parent_print_sync_set_status_callback(parent_print_sync_status_fn_t fn, void *user_data)
{
    s_status_fn = fn;
    s_status_user = user_data;
}

uint32_t parent_print_sync_pending_count(void)
{
    return s_pending;
}

size_t parent_print_sync_job_count(void)
{
    return s_jobs.size();
}

bool parent_print_sync_get_job(size_t index, parent_print_job_t *out)
{
    if (out == nullptr || index >= s_jobs.size()) {
        return false;
    }
    *out = s_jobs[index];
    return true;
}

static bool parse_poll_body(const std::string &body)
{
    s_jobs.clear();
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (cJSON_IsArray(items)) {
        const int n = cJSON_GetArraySize(items);
        for (int i = 0; i < n; i++) {
            const cJSON *it = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(it)) {
                continue;
            }
            parent_print_job_t job = {};
            job.text_title[0] = '\0';
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
            const cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "type");
            const cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
            const cJSON *js = cJSON_GetObjectItemCaseSensitive(it, "size");
            const cJSON *jc = cJSON_GetObjectItemCaseSensitive(it, "created_at");
            if (!cJSON_IsNumber(jid)) {
                continue;
            }
            job.id = jid->valueint;
            if (cJSON_IsString(jt) && jt->valuestring) {
                if (strcmp(jt->valuestring, "text") == 0) {
                    job.type = PARENT_PRINT_JOB_TYPE_TEXT;
                } else {
                    job.type = PARENT_PRINT_JOB_TYPE_IMAGE;
                }
            } else {
                job.type = PARENT_PRINT_JOB_TYPE_IMAGE;
            }
            if (cJSON_IsString(jn) && jn->valuestring) {
                snprintf(job.name, sizeof(job.name), "%s", jn->valuestring);
            } else {
                snprintf(job.name, sizeof(job.name), "job_%d", job.id);
            }
            if (cJSON_IsNumber(js)) {
                job.size = (size_t)js->valuedouble;
            }
            if (cJSON_IsNumber(jc)) {
                job.created_at = jc->valuedouble;
            }
            if (job.type == PARENT_PRINT_JOB_TYPE_TEXT) {
                const cJSON *jtitle = cJSON_GetObjectItemCaseSensitive(it, "title");
                if (cJSON_IsString(jtitle) && jtitle->valuestring && jtitle->valuestring[0] != '\0') {
                    snprintf(job.text_title, sizeof(job.text_title), "%s", jtitle->valuestring);
                } else if (job.name[0] != '\0' && strcmp(job.name, "文字") != 0 &&
                           strncmp(job.name, "job_", 4) != 0) {
                    snprintf(job.text_title, sizeof(job.text_title), "%s", job.name);
                }
            }
            s_jobs.push_back(job);
        }
    }
    s_pending = (uint32_t)s_jobs.size();
    cJSON_Delete(root);
    return true;
}

esp_err_t parent_print_sync_refresh_pending(void)
{
    std::string body;
    if (!http_get_text("/parent/api/print/poll", body, nullptr)) {
        return ESP_FAIL;
    }
    if (!parse_poll_body(body)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const parent_print_job_t *find_job(int job_id)
{
    for (const auto &j : s_jobs) {
        if (j.id == job_id) {
            return &j;
        }
    }
    return nullptr;
}

static esp_err_t print_text_job(const parent_print_job_t &job)
{
    char path[96];
    snprintf(path, sizeof(path), "/parent/api/print/text/%d", job.id);
    std::string body;
    if (!http_get_text(path, body, nullptr)) {
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return ESP_FAIL;
    }
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (!cJSON_IsString(txt) || txt->valuestring == nullptr) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    std::string to_print = txt->valuestring;
    cJSON_Delete(root);
    esp_err_t err = escpos_printer_print_utf8(to_print.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "text print err=%s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t print_image_job(const parent_print_job_t &job)
{
    if (job.size == 0 || job.size > PARENT_PRINT_MAX_FILE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!parent_print_fs_is_mounted()) {
        ESP_LOGE(TAG, "no storage for image job");
        return ESP_ERR_INVALID_STATE;
    }
    if (!parent_print_fs_has_space_for(job.size)) {
        ESP_LOGE(TAG, "storage full for image job");
        return ESP_ERR_NO_MEM;
    }

    char part_path[PARENT_PRINT_PATH_MAX];
    esp_err_t err = parent_print_fs_make_part_path(job.id, part_path, sizeof(part_path));
    if (err != ESP_OK) {
        return err;
    }
    unlink(part_path);

    ESP_LOGD(TAG, "download_start id=%d bytes=%u", job.id, (unsigned)job.size);

    FILE *f = fopen(part_path, "wb");
    if (f == nullptr) {
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(PARENT_PRINT_PART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        buf = (uint8_t *)malloc(PARENT_PRINT_PART_BYTES);
    }
    if (buf == nullptr) {
        fclose(f);
        unlink(part_path);
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    char qpath[128];
    while (off < job.size) {
        size_t chunk = job.size - off;
        if (chunk > PARENT_PRINT_PART_BYTES) {
            chunk = PARENT_PRINT_PART_BYTES;
        }
        snprintf(qpath, sizeof(qpath), "/parent/api/print/file/%d?off=%u&len=%u", job.id, (unsigned)off,
                 (unsigned)chunk);
        size_t got = 0;
        if (!http_get_binary_chunk(qpath, chunk, buf, &got) || got == 0) {
            ESP_LOGE(TAG, "chunk fail id=%d off=%u", job.id, (unsigned)off);
            free(buf);
            fclose(f);
            unlink(part_path);
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, got, f) != got) {
            free(buf);
            fclose(f);
            unlink(part_path);
            return ESP_FAIL;
        }
        if (got != chunk) {
            ESP_LOGW(TAG, "chunk short id=%d off=%u want=%u got=%u", job.id, (unsigned)off, (unsigned)chunk,
                     (unsigned)got);
        }
        off += got;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    free(buf);
    fclose(f);
    ESP_LOGD(TAG, "download_done id=%d bytes=%u", job.id, (unsigned)off);

    if (off != job.size) {
        unlink(part_path);
        return ESP_FAIL;
    }

    FILE *vf = fopen(part_path, "rb");
    if (vf != nullptr) {
        uint8_t hdr[2];
        if (fread(hdr, 1, 2, vf) != 2 || hdr[0] != 0xff || hdr[1] != 0xd8) {
            fclose(vf);
            unlink(part_path);
            return ESP_ERR_INVALID_ARG;
        }
        fclose(vf);
    }

    ESP_LOGD(TAG, "raster_begin id=%d path=%s", job.id, part_path);
    err = escpos_printer_print_jpeg_file(part_path);
    unlink(part_path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg print err=%s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGD(TAG, "raster_done id=%d err=%s", job.id, esp_err_to_name(err));
    return ESP_OK;
}

static bool ack_job(int job_id)
{
    char ack[48];
    snprintf(ack, sizeof(ack), "{\"id\":%d}", job_id);
    std::string resp;
    if (!http_post_json("/parent/api/print/ack", ack, resp)) {
        ESP_LOGW(TAG, "ack failed id=%d", job_id);
        return false;
    }
    return true;
}

esp_err_t parent_print_sync_print_job(int job_id)
{
    if (s_job_mux == nullptr) {
        s_job_mux = xSemaphoreCreateMutex();
        if (s_job_mux == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_job_mux, portMAX_DELAY);
    if (s_busy) {
        xSemaphoreGive(s_job_mux);
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    xSemaphoreGive(s_job_mux);

    if (parent_print_sync_refresh_pending() != ESP_OK) {
        xSemaphoreTake(s_job_mux, portMAX_DELAY);
        s_busy = false;
        xSemaphoreGive(s_job_mux);
        return ESP_FAIL;
    }

    const parent_print_job_t *job = find_job(job_id);
    if (job == nullptr) {
        xSemaphoreTake(s_job_mux, portMAX_DELAY);
        s_busy = false;
        xSemaphoreGive(s_job_mux);
        return ESP_ERR_NOT_FOUND;
    }

    focus_enter();
    esp_err_t err = ESP_OK;
    if (job->type == PARENT_PRINT_JOB_TYPE_TEXT) {
        err = print_text_job(*job);
    } else {
        err = print_image_job(*job);
    }

    if (err == ESP_OK) {
        (void)ack_job(job_id);
        char line[96];
        snprintf(line, sizeof(line), "已打印 %s", job->name);
        notify_status(line);
        (void)parent_print_sync_refresh_pending();
    } else {
        if (err == ESP_ERR_INVALID_STATE) {
            notify_status("打印失败：无存储");
        } else if (err == ESP_ERR_NO_MEM) {
            notify_status("打印失败：空间不足");
        } else if (err == ESP_ERR_NOT_FOUND) {
            notify_status("打印失败：无打印机");
        } else {
            notify_status("打印失败");
        }
    }

    focus_exit();
    xSemaphoreTake(s_job_mux, portMAX_DELAY);
    s_busy = false;
    xSemaphoreGive(s_job_mux);
    return err;
}

bool parent_print_sync_is_busy(void)
{
    return s_busy;
}

static void bg_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(25000));
    while (s_bg_run) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        (void)parent_print_sync_refresh_pending();
        vTaskDelay(pdMS_TO_TICKS(12000));
    }
    s_bg_task = nullptr;
    vTaskDelete(nullptr);
}

void parent_print_sync_bg_start(void)
{
    if (s_bg_task != nullptr) {
        return;
    }
    s_bg_run = true;
    xTaskCreate(bg_task, "prt_sync_bg", 16384, nullptr, 3, &s_bg_task);
}

void parent_print_sync_bg_stop(void)
{
    s_bg_run = false;
}

void parent_print_sync_pause(bool paused)
{
    s_paused = paused;
    if (paused) {
        ESP_LOGI(TAG, "background poll paused");
    } else {
        ESP_LOGI(TAG, "background poll resumed");
    }
}
