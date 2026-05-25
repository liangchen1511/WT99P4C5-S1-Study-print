/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_chat_api.hpp"
#include "parent_chat_launcher.hpp"

#include "soti_config.h"

#include <cstring>
#include <cstdio>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "parent_chat";

static TaskHandle_t s_bg_task;
static volatile bool s_bg_run;
static volatile bool s_bg_paused;

static void http_client_cfg_tls_if_https(esp_http_client_config_t *cfg, const char *url)
{
    if (cfg == nullptr || url == nullptr) {
        return;
    }
    if (strncmp(url, "https://", 8) != 0) {
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
    esp_http_client_set_header(client, "Content-Type", "application/json");
}

bool parent_chat_build_api_url(char *dst, size_t cap, const char *path)
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

static constexpr size_t k_http_body_cap = 8192;

/** 读 HTTP body 到固定缓冲，避免 std::string::resize 在 OOM 时触发 abort()。 */
static int http_read_body_to_buf(esp_http_client_handle_t client, char *buf, size_t cap)
{
    if (buf == nullptr || cap < 2) {
        return -1;
    }
    int total = 0;
    const int max_read = (int)cap - 1;
    while (total < max_read) {
        int r = esp_http_client_read(client, buf + total, max_read - total);
        if (r <= 0) {
            break;
        }
        total += r;
    }
    buf[total] = '\0';
    return total;
}

static bool http_read_body(esp_http_client_handle_t client, std::string &out)
{
    out.clear();
    char *buf = (char *)heap_caps_malloc(k_http_body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        buf = (char *)heap_caps_malloc(k_http_body_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buf == nullptr) {
        ESP_LOGW(TAG, "http_read_body: no buffer");
        return false;
    }
    const int total = http_read_body_to_buf(client, buf, k_http_body_cap);
    if (total < 0) {
        heap_caps_free(buf);
        return false;
    }
    const size_t need = (size_t)total + 1;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < need + 4096) {
        ESP_LOGW(TAG, "http_read_body: low heap, skip std::string (%d bytes)", total);
        heap_caps_free(buf);
        return false;
    }
    out.assign(buf, (size_t)total);
    heap_caps_free(buf);
    return true;
}

static bool parent_chat_http_get_buf(const char *path_with_query, char *body, size_t cap, int *out_status)
{
    if (body == nullptr || cap < 64) {
        return false;
    }
    body[0] = '\0';
    char url[320];
    if (!parent_chat_build_api_url(url, sizeof(url), path_with_query)) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;
    http_client_cfg_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    apply_bearer(client);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    (void)http_read_body_to_buf(client, body, cap);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (out_status != nullptr) {
        *out_status = status;
    }
    return status == 200;
}

bool parent_chat_http_get(const char *path_with_query, std::string &out_body, int *out_status)
{
    char url[320];
    if (!parent_chat_build_api_url(url, sizeof(url), path_with_query)) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;
    http_client_cfg_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    apply_bearer(client);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    (void)esp_http_client_fetch_headers(client);
    http_read_body(client, out_body);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (out_status) {
        *out_status = status;
    }
    return status == 200;
}

bool parent_chat_http_post_json(const char *path, const char *json_body, std::string &out_body, int *out_status)
{
    char url[320];
    if (!parent_chat_build_api_url(url, sizeof(url), path)) {
        return false;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 12000;
    cfg.method = HTTP_METHOD_POST;
    http_client_cfg_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return false;
    }
    apply_bearer(client);
    const char *body = json_body ? json_body : "{}";
    int blen = (int)strlen(body);
    esp_err_t err = esp_http_client_open(client, blen);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    if (blen > 0) {
        (void)esp_http_client_write(client, body, blen);
    }
    (void)esp_http_client_fetch_headers(client);
    http_read_body(client, out_body);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (out_status) {
        *out_status = status;
    }
    return status == 200;
}

static bool parse_messages(const cJSON *items, std::vector<ParentChatMsg> &out)
{
    if (!cJSON_IsArray(items)) {
        return false;
    }
    const int n = cJSON_GetArraySize(items);
    for (int i = 0; i < n; i++) {
        const cJSON *it = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(it)) {
            continue;
        }
        ParentChatMsg m;
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(it, "sender");
        const cJSON *jb = cJSON_GetObjectItemCaseSensitive(it, "body");
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "created_at");
        if (cJSON_IsNumber(jid)) {
            m.id = jid->valueint;
        }
        if (cJSON_IsString(js) && js->valuestring) {
            m.sender = js->valuestring;
        }
        if (cJSON_IsString(jb) && jb->valuestring) {
            m.body = jb->valuestring;
        }
        if (cJSON_IsNumber(jt)) {
            m.created_at = jt->valuedouble;
        }
        out.push_back(m);
    }
    return true;
}

static bool parse_unread(const cJSON *root, ParentChatUnread *unread)
{
    if (unread == nullptr) {
        return true;
    }
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(root, "unread");
    if (!cJSON_IsObject(u)) {
        u = root;
    }
    const cJSON *pu = cJSON_GetObjectItemCaseSensitive(u, "parent_unread");
    const cJSON *cu = cJSON_GetObjectItemCaseSensitive(u, "child_unread");
    const cJSON *li = cJSON_GetObjectItemCaseSensitive(u, "latest_id");
    if (cJSON_IsNumber(pu)) {
        unread->parent_unread = pu->valueint;
    }
    if (cJSON_IsNumber(cu)) {
        unread->child_unread = cu->valueint;
    }
    if (cJSON_IsNumber(li)) {
        unread->latest_id = li->valueint;
    }
    return true;
}

bool parent_chat_fetch_recent(std::vector<ParentChatMsg> &out, ParentChatUnread *unread)
{
    std::string body;
    if (!parent_chat_http_get("/parent/api/chat/messages?limit=50", body, nullptr)) {
        return false;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    out.clear();
    parse_messages(cJSON_GetObjectItemCaseSensitive(root, "items"), out);
    parse_unread(root, unread);
    cJSON_Delete(root);
    return true;
}

bool parent_chat_poll(int since_id, std::vector<ParentChatMsg> &out, ParentChatUnread *unread)
{
    char q[96];
    snprintf(q, sizeof(q), "/parent/api/chat/poll?since=%d&limit=50", since_id);
    std::string body;
    if (!parent_chat_http_get(q, body, nullptr)) {
        return false;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }
    out.clear();
    parse_messages(cJSON_GetObjectItemCaseSensitive(root, "items"), out);
    parse_unread(root, unread);
    cJSON_Delete(root);
    return true;
}

bool parent_chat_send(const char *body, const char *client_msg_id, ParentChatMsg *out_msg)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "body", body ? body : "");
    cJSON_AddStringToObject(req, "sender", "child");
    if (client_msg_id && client_msg_id[0]) {
        cJSON_AddStringToObject(req, "client_msg_id", client_msg_id);
    }
    char *printed = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (printed == nullptr) {
        return false;
    }
    std::string resp;
    bool ok = parent_chat_http_post_json("/parent/api/chat/send", printed, resp, nullptr);
    cJSON_free(printed);
    if (!ok) {
        return false;
    }
    cJSON *root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        return false;
    }
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (out_msg && cJSON_IsObject(msg)) {
        ParentChatMsg one;
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(msg, "id");
        const cJSON *js = cJSON_GetObjectItemCaseSensitive(msg, "sender");
        const cJSON *jb = cJSON_GetObjectItemCaseSensitive(msg, "body");
        if (cJSON_IsNumber(jid)) {
            one.id = jid->valueint;
        }
        if (cJSON_IsString(js) && js->valuestring) {
            one.sender = js->valuestring;
        }
        if (cJSON_IsString(jb) && jb->valuestring) {
            one.body = jb->valuestring;
        }
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(msg, "created_at");
        if (cJSON_IsNumber(jt)) {
            one.created_at = jt->valuedouble;
        }
        *out_msg = one;
    }
    ParentChatUnread unread = {};
    parse_unread(root, &unread);
    parent_chat_launcher_set_unread(unread.child_unread > 0);
    cJSON_Delete(root);
    return true;
}

bool parent_chat_mark_read_child(int up_to_id)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"role\":\"child\",\"up_to_id\":%d}", up_to_id);
    std::string resp;
    if (!parent_chat_http_post_json("/parent/api/chat/read", json, resp, nullptr)) {
        return false;
    }
    parent_chat_launcher_set_unread(false);
    return true;
}

bool parent_chat_fetch_unread(ParentChatUnread *unread)
{
    char *body = (char *)heap_caps_malloc(k_http_body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == nullptr) {
        body = (char *)heap_caps_malloc(k_http_body_cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (body == nullptr) {
        ESP_LOGW(TAG, "fetch_unread: OOM");
        return false;
    }
    if (!parent_chat_http_get_buf("/parent/api/chat/unread", body, k_http_body_cap, nullptr)) {
        heap_caps_free(body);
        return false;
    }
    cJSON *root = cJSON_Parse(body);
    if (root == nullptr) {
        heap_caps_free(body);
        return false;
    }
    if (unread) {
        parse_unread(root, unread);
    }
    cJSON_Delete(root);
    heap_caps_free(body);
    return true;
}

static void bg_poll_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));
    while (s_bg_run) {
        if (s_bg_paused) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ParentChatUnread u = {};
        if (parent_chat_fetch_unread(&u)) {
            parent_chat_launcher_set_unread(u.child_unread > 0);
        }
        vTaskDelay(pdMS_TO_TICKS(12000));
    }
    s_bg_task = nullptr;
    vTaskDelete(nullptr);
}

void parent_chat_bg_poll_start(void)
{
    if (s_bg_task != nullptr) {
        return;
    }
    s_bg_run = true;
    xTaskCreate(bg_poll_task, "pchat_bg", 8192, nullptr, 3, &s_bg_task);
}

void parent_chat_bg_poll_stop(void)
{
    s_bg_run = false;
    // If task is blocked in vTaskDelay, we can't wake it; caller should wait
}

void parent_chat_bg_pause(bool paused)
{
    s_bg_paused = paused;
    if (paused) {
        ESP_LOGI(TAG, "bg poll paused (SoTi upload / low heap)");
    } else {
        ESP_LOGI(TAG, "bg poll resumed");
    }
}

bool parent_chat_bg_poll_is_running(void)
{
    return s_bg_task != nullptr;
}

void parent_chat_bg_poll_wait_stop(int max_wait_ms)
{
    const int step_ms = 50;
    for (int waited = 0; waited < max_wait_ms && s_bg_task != nullptr; waited += step_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}
