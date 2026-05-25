/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_album_sync.h"
#include "parent_album_fs.h"

#include "soti_config.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_manager.h"

static const char *TAG = "alb_sync";

static parent_album_sync_hooks_t s_hooks;
static parent_album_sync_status_fn_t s_status_fn;
static void *s_status_user;
static uint32_t s_pending;
static TaskHandle_t s_bg_task;
static volatile bool s_bg_run;
static volatile bool s_sync_busy;
static volatile bool s_paused;

static void notify_status(const char *line)
{
    if (s_status_fn != nullptr && line != nullptr) {
        s_status_fn(line, s_status_user);
    }
}

void parent_album_sync_set_hooks(const parent_album_sync_hooks_t *hooks)
{
    if (hooks == nullptr) {
        memset(&s_hooks, 0, sizeof(s_hooks));
        return;
    }
    s_hooks = *hooks;
}

void parent_album_sync_set_status_callback(parent_album_sync_status_fn_t fn, void *user_data)
{
    s_status_fn = fn;
    s_status_user = user_data;
}

uint32_t parent_album_sync_pending_count(void)
{
    return s_pending;
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
    out.resize(4096);
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

static bool http_get_binary_chunk(const char *path_query, size_t offset, size_t len, uint8_t *buf, size_t *got)
{
    *got = 0;
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
    int total = 0;
    while (total < (int)len) {
        int chunk = (int)len - total;
        if (chunk > 512) {
            chunk = 512;
        }
        int r = esp_http_client_read(client, (char *)buf + total, chunk);
        if (r == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
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

struct PendingItem {
    int id;
    std::string name;
    size_t size;
};

static wifi_ps_type_t s_saved_ps;
static bool s_saved_ps_valid;
static bool s_focus_on;

static void focus_enter(void)
{
    if (s_focus_on) {
        return;
    }
    s_focus_on = true;
    s_saved_ps_valid = (esp_wifi_get_ps(&s_saved_ps) == ESP_OK);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);
    power_manager_block_sleep("alb_sync");
    if (s_hooks.on_enter != nullptr) {
        s_hooks.on_enter();
    }
}

static void focus_exit(void)
{
    if (!s_focus_on) {
        return;
    }
    if (s_hooks.on_exit != nullptr) {
        s_hooks.on_exit();
    }
    power_manager_unblock_sleep("alb_sync");
    if (s_saved_ps_valid) {
        (void)esp_wifi_set_ps(s_saved_ps);
    }
    s_focus_on = false;
}

static bool poll_pending(std::vector<PendingItem> &out)
{
    out.clear();
    std::string body;
    if (!http_get_text("/parent/api/album/poll", body, nullptr)) {
        return false;
    }
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
            PendingItem p;
            const cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
            const cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
            const cJSON *js = cJSON_GetObjectItemCaseSensitive(it, "size");
            if (!cJSON_IsNumber(jid)) {
                continue;
            }
            p.id = jid->valueint;
            if (cJSON_IsString(jn) && jn->valuestring) {
                p.name = jn->valuestring;
            }
            if (cJSON_IsNumber(js)) {
                p.size = (size_t)js->valuedouble;
            }
            out.push_back(p);
        }
    }
    s_pending = (uint32_t)out.size();
    cJSON_Delete(root);
    return true;
}

esp_err_t parent_album_sync_refresh_pending(void)
{
    std::vector<PendingItem> pending;
    if (!poll_pending(pending)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t download_one(const PendingItem &item)
{
    if (item.size == 0 || item.size > PARENT_ALBUM_MAX_FILE) {
        ESP_LOGW(TAG, "skip id=%d bad size %u", item.id, (unsigned)item.size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!parent_album_fs_has_space_for(item.size)) {
        ESP_LOGW(TAG, "no space for id=%d", item.id);
        return ESP_ERR_NO_MEM;
    }

    char final_path[PARENT_ALBUM_PATH_MAX];
    esp_err_t err = parent_album_fs_pick_final_path(
        item.name.empty() ? nullptr : item.name.c_str(), final_path, sizeof(final_path));
    if (err != ESP_OK) {
        return err;
    }

    char part_path[PARENT_ALBUM_PATH_MAX];
    parent_album_fs_make_part_path(item.id, part_path, sizeof(part_path));
    unlink(part_path);

    FILE *f = fopen(part_path, "wb");
    if (f == nullptr) {
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc(PARENT_ALBUM_PART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        buf = (uint8_t *)malloc(PARENT_ALBUM_PART_BYTES);
    }
    if (buf == nullptr) {
        fclose(f);
        unlink(part_path);
        return ESP_ERR_NO_MEM;
    }

    size_t off = 0;
    char qpath[128];
    while (off < item.size) {
        size_t chunk = item.size - off;
        if (chunk > PARENT_ALBUM_PART_BYTES) {
            chunk = PARENT_ALBUM_PART_BYTES;
        }
        snprintf(qpath, sizeof(qpath), "/parent/api/album/file/%d?off=%u&len=%u", item.id, (unsigned)off,
                 (unsigned)chunk);
        size_t got = 0;
        if (!http_get_binary_chunk(qpath, off, chunk, buf, &got) || got == 0) {
            ESP_LOGE(TAG, "chunk fail id=%d off=%u", item.id, (unsigned)off);
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
        off += got;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    free(buf);
    fclose(f);

    if (off != item.size) {
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

    unlink(final_path);
    if (rename(part_path, final_path) != 0) {
        ESP_LOGE(TAG, "rename failed %s", final_path);
        unlink(part_path);
        return ESP_FAIL;
    }

    char ack[48];
    snprintf(ack, sizeof(ack), "{\"id\":%d}", item.id);
    std::string resp;
    if (!http_post_json("/parent/api/album/ack", ack, resp)) {
        ESP_LOGW(TAG, "ack failed id=%d", item.id);
    }

    const char *slash = strrchr(final_path, '/');
    char line[96];
    snprintf(line, sizeof(line), "已同步 %s", slash ? slash + 1 : final_path);
    notify_status(line);
    ESP_LOGI(TAG, "saved %s", final_path);
    return ESP_OK;
}

esp_err_t parent_album_sync_run_once(void)
{
    if (s_sync_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!parent_album_fs_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sync_busy = true;

    std::vector<PendingItem> pending;
    if (!poll_pending(pending)) {
        s_sync_busy = false;
        return ESP_FAIL;
    }
    if (pending.empty()) {
        notify_status("无待同步照片");
        s_sync_busy = false;
        return ESP_OK;
    }

    focus_enter();
    esp_err_t last = ESP_OK;
    for (const auto &it : pending) {
        esp_err_t e = download_one(it);
        if (e != ESP_OK) {
            last = e;
        }
    }
    focus_exit();
    (void)parent_album_sync_refresh_pending();
    s_sync_busy = false;
    return last;
}

static void bg_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(8000));
    while (s_bg_run) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        std::vector<PendingItem> pending;
        if (poll_pending(pending) && !pending.empty()) {
            ESP_LOGI(TAG, "bg: %u pending album", (unsigned)pending.size());
            parent_album_sync_run_once();
        }
        vTaskDelay(pdMS_TO_TICKS(12000));
    }
    s_bg_task = nullptr;
    vTaskDelete(nullptr);
}

void parent_album_sync_bg_start(void)
{
    if (s_bg_task != nullptr) {
        return;
    }
    s_bg_run = true;
    xTaskCreate(bg_task, "alb_sync_bg", 16384, nullptr, 3, &s_bg_task);
}

void parent_album_sync_bg_stop(void)
{
    s_bg_run = false;
}

void parent_album_sync_pause(bool paused)
{
    s_paused = paused;
    if (paused) {
        ESP_LOGI(TAG, "background sync paused (SoTi upload)");
    } else {
        ESP_LOGI(TAG, "background sync resumed");
    }
}
