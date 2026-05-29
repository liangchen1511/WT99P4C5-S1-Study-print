/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_policy.hpp"
#include "parent_config.h"
#include "parent_net_gate.h"
#include "soti_config.h"

#include <cstring>
#include <ctime>
#include <string>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "parent_pol";
static const char *NVS_NS = "parent_pol";
static const char *NVS_KEY_JSON = "policy";

static char s_policy_json[4096];
static bool s_demo_force_study;
static bool s_have_policy;
static char s_block_reason[96];
static TaskHandle_t s_poll_task;
static volatile bool s_poll_paused = true;

static void set_block_reason(const char *msg)
{
    if (msg == nullptr) {
        s_block_reason[0] = '\0';
        return;
    }
    strncpy(s_block_reason, msg, sizeof(s_block_reason) - 1);
    s_block_reason[sizeof(s_block_reason) - 1] = '\0';
}

static bool build_policy_url(char *dst, size_t cap)
{
    const char *base = SOTI_R2_WORKER_URL;
    if (base == nullptr || cap < 64) {
        return false;
    }
    const char *suffix = "/upload";
    size_t lu = strlen(base);
    size_t ls = strlen(suffix);
    if (lu >= ls && strcmp(base + lu - ls, suffix) == 0) {
        size_t prefix = lu - ls;
        int n = snprintf(dst, cap, "%.*s/parent/api/policy?device_id=%s", (int)prefix, base, SOTI_DEVICE_ID);
        return n > 0 && (size_t)n < cap;
    }
    int n = snprintf(dst, cap, "%s/parent/api/policy?device_id=%s", base, SOTI_DEVICE_ID);
    return n > 0 && (size_t)n < cap;
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

static bool load_policy_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(s_policy_json);
    esp_err_t err = nvs_get_str(h, NVS_KEY_JSON, s_policy_json, &len);
    nvs_close(h);
    if (err != ESP_OK || s_policy_json[0] == '\0') {
        return false;
    }
    return true;
}

static bool save_policy_nvs(const char *json)
{
    if (json == nullptr) {
        return false;
    }
    strncpy(s_policy_json, json, sizeof(s_policy_json) - 1);
    s_policy_json[sizeof(s_policy_json) - 1] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_str(h, NVS_KEY_JSON, s_policy_json);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

static int parse_hhmm(const char *s)
{
    if (s == nullptr) {
        return -1;
    }
    int h = 0;
    int m = 0;
    if (sscanf(s, "%d:%d", &h, &m) != 2) {
        return -1;
    }
    return h * 60 + m;
}

static int iso_weekday_from_tm(const struct tm *t)
{
    if (t == nullptr) {
        return 1;
    }
    /* tm_wday: 0=Sun .. 6=Sat -> ISO 1=Mon .. 7=Sun */
    return t->tm_wday == 0 ? 7 : t->tm_wday;
}

static bool json_has_int_in_array(const cJSON *arr, int val)
{
    if (!cJSON_IsArray(arr)) {
        return false;
    }
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr)
    {
        if (cJSON_IsNumber(it) && it->valueint == val) {
            return true;
        }
    }
    return false;
}

static bool app_in_json_array(const cJSON *arr, const char *app_id)
{
    if (!cJSON_IsArray(arr) || app_id == nullptr) {
        return false;
    }
    const cJSON *it = nullptr;
    cJSON_ArrayForEach(it, arr)
    {
        if (cJSON_IsString(it) && it->valuestring != nullptr) {
            if (strcmp(it->valuestring, "*") == 0 || strcmp(it->valuestring, app_id) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void parse_policy_flags(void)
{
    s_demo_force_study = false;
    s_have_policy = false;
    cJSON *root = cJSON_Parse(s_policy_json);
    if (root == nullptr) {
        return;
    }
    s_have_policy = true;
    const cJSON *df = cJSON_GetObjectItemCaseSensitive(root, "demo_force_study");
    if (cJSON_IsTrue(df)) {
        s_demo_force_study = true;
    }
    cJSON_Delete(root);
}

static bool toggle_allows(const char *app_id)
{
    cJSON *root = cJSON_Parse(s_policy_json);
    if (root == nullptr) {
        return true;
    }
    const cJSON *toggles = cJSON_GetObjectItemCaseSensitive(root, "app_toggles");
    if (!cJSON_IsObject(toggles)) {
        cJSON_Delete(root);
        return true;
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(toggles, app_id);
    bool allow = true;
    if (cJSON_IsBool(v)) {
        allow = cJSON_IsTrue(v);
    }
    cJSON_Delete(root);
    return allow;
}

static bool rule_matches_now(const cJSON *rule)
{
    if (rule == nullptr) {
        return false;
    }
    time_t now = time(nullptr);
    struct tm t;
    if (localtime_r(&now, &t) == nullptr) {
        return false;
    }
    int iso_day = iso_weekday_from_tm(&t);
    const cJSON *days = cJSON_GetObjectItemCaseSensitive(rule, "days");
    if (cJSON_IsArray(days) && cJSON_GetArraySize(days) > 0 && !json_has_int_in_array(days, iso_day)) {
        return false;
    }
    int now_min = t.tm_hour * 60 + t.tm_min;
    const cJSON *start = cJSON_GetObjectItemCaseSensitive(rule, "start");
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(rule, "end");
    int s0 = parse_hhmm(cJSON_IsString(start) ? start->valuestring : "00:00");
    int e0 = parse_hhmm(cJSON_IsString(end) ? end->valuestring : "23:59");
    if (s0 < 0 || e0 < 0) {
        return true;
    }
    if (s0 <= e0) {
        return now_min >= s0 && now_min < e0;
    }
    return now_min >= s0 || now_min < e0;
}

static bool schedule_allows(const char *app_id)
{
    cJSON *root = cJSON_Parse(s_policy_json);
    if (root == nullptr) {
        return true;
    }
    const cJSON *rules = cJSON_GetObjectItemCaseSensitive(root, "rules");
    bool allowed = true;
    if (cJSON_IsArray(rules)) {
        const cJSON *rule = nullptr;
        bool any_match = false;
        cJSON_ArrayForEach(rule, rules)
        {
            if (!rule_matches_now(rule)) {
                continue;
            }
            any_match = true;
            const cJSON *deny = cJSON_GetObjectItemCaseSensitive(rule, "deny_apps");
            if (app_in_json_array(deny, app_id)) {
                allowed = false;
                const cJSON *name = cJSON_GetObjectItemCaseSensitive(rule, "name");
                if (cJSON_IsString(name) && name->valuestring != nullptr) {
                    snprintf(s_block_reason, sizeof(s_block_reason), "当前为%s", name->valuestring);
                } else {
                    set_block_reason("当前时段不可使用");
                }
                break;
            }
            const cJSON *allow = cJSON_GetObjectItemCaseSensitive(rule, "allow_apps");
            if (cJSON_IsArray(allow) && cJSON_GetArraySize(allow) > 0) {
                if (!app_in_json_array(allow, app_id)) {
                    allowed = false;
                    set_block_reason("当前时段仅开放学习应用");
                    break;
                }
            }
        }
        if (!any_match) {
            const cJSON *lock = cJSON_GetObjectItemCaseSensitive(root, "device_lock_outside_rules");
            if (cJSON_IsTrue(lock)) {
                allowed = false;
                set_block_reason("当前不在允许使用时段");
            }
        }
    }
    cJSON_Delete(root);
    return allowed;
}

static esp_err_t fetch_policy_http(void)
{
    if (!parent_net_http_allowed()) {
        return ESP_ERR_INVALID_STATE;
    }
    char url[280];
    if (!build_policy_url(url, sizeof(url))) {
        return ESP_FAIL;
    }
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = 15000;
    http_client_cfg_tls_if_https(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    apply_bearer(client);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int len = esp_http_client_fetch_headers(client);
    if (len < 0) {
        len = 0;
    }
    std::string body;
    body.resize(4096);
    int total = 0;
    while (total < (int)body.size() - 1) {
        int r = esp_http_client_read(client, &body[total], (int)body.size() - 1 - total);
        if (r < 0) {
            break;
        }
        if (r == 0) {
            break;
        }
        total += r;
    }
    body[total] = '\0';
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (status != 200) {
        ESP_LOGW(TAG, "policy HTTP %d", status);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return ESP_FAIL;
    }
    const cJSON *pol = cJSON_GetObjectItemCaseSensitive(root, "policy");
    if (!cJSON_IsObject(pol)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    char *printed = cJSON_PrintUnformatted(pol);
    cJSON_Delete(root);
    if (printed == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    save_policy_nvs(printed);
    cJSON_free(printed);
    parse_policy_flags();
    ESP_LOGI(TAG, "policy synced");
    return ESP_OK;
}

static void poll_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(20000));
    for (;;) {
        if (!s_poll_paused) {
            if (fetch_policy_http() != ESP_OK) {
                ESP_LOGD(TAG, "policy fetch skip (offline or server)");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PARENT_POLICY_POLL_MS));
    }
}

extern "C" void parent_policy_poll_pause(bool paused)
{
    s_poll_paused = paused;
}

extern "C" void parent_policy_init(void)
{
    set_block_reason("");
    if (load_policy_nvs()) {
        parse_policy_flags();
        ESP_LOGI(TAG, "policy loaded from NVS");
    }
    if (s_poll_task == nullptr) {
        xTaskCreate(poll_task, "parent_pol", 8192, nullptr, 3, &s_poll_task);
    }
}

extern "C" bool parent_policy_is_app_allowed(const char *app_id)
{
    if (app_id == nullptr || app_id[0] == '\0') {
        return true;
    }
    if (strcmp(app_id, "settings") == 0 || strcmp(app_id, "calculator") == 0) {
        return true;
    }
    set_block_reason("");
    if (s_demo_force_study) {
        if (strcmp(app_id, "game_2048") == 0 || strcmp(app_id, "video") == 0 || strcmp(app_id, "music") == 0 ||
            strcmp(app_id, "camera") == 0 || strcmp(app_id, "print") == 0) {
            set_block_reason("学习时段：非学习应用已锁定");
            return false;
        }
    }
    if (s_have_policy && !toggle_allows(app_id)) {
        set_block_reason("家长已关闭此应用");
        return false;
    }
    if (s_have_policy && !schedule_allows(app_id)) {
        if (s_block_reason[0] == '\0') {
            set_block_reason("当前时段不可使用");
        }
        return false;
    }
    return true;
}

extern "C" const char *parent_policy_block_reason(void)
{
    return s_block_reason[0] ? s_block_reason : "当前不可使用";
}
