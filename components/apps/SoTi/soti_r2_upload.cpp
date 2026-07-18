/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soti_r2_upload.hpp"
#include "soti_config.h"
#include "soti_print_sections.hpp"

#include <cstring>
#include <cstdio>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SoTiR2";

/** 单连接 POST 时的单块大小（分片模式不用此上限发满 TCP）。 */
static constexpr int k_body_write_chunk = 1024;
static constexpr int k_stage_bytes = 1024;
/** 分片每 TCP 请求 body 最大字节（SDIO mempool 紧张时须更小，见串口 mempool_alloc failed）。 */
static constexpr int k_segment_part_bytes = 1024;
/** 分片 part 之间间隔，给 ESP-Hosted SDIO TX 队列释放 mbuf 的时间。 */
static constexpr int k_segment_part_gap_ms = 120;
/** commit JSON 响应缓冲（与 answer_out 16KB 一致，优先 PSRAM 减轻内部堆压力）。 */
static constexpr size_t k_resp_body_cap = 16384;
static constexpr size_t k_upload_url_cap = 280;

static char *alloc_response_body(size_t cap)
{
    char *p = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = (char *)heap_caps_malloc(cap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void copy_trunc_utf8(char *dst, size_t dst_sz, const char *src)
{
    if (dst == nullptr || dst_sz == 0) {
        return;
    }
    dst[0] = '\0';
    if (src == nullptr) {
        return;
    }
    size_t n = 0;
    while (n < dst_sz - 1 && src[n] != '\0') {
        n++;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool build_worker_url_with_mode(char *dst, size_t cap, const char *mode)
{
    if (dst == nullptr || cap == 0) {
        return false;
    }
    const char *base = SOTI_R2_WORKER_URL;
    if (mode == nullptr || mode[0] == '\0' || strcmp(mode, "solve") == 0) {
        copy_trunc_utf8(dst, cap, base);
        return true;
    }
    const char *q = strchr(base, '?');
    int n = (q != nullptr) ? snprintf(dst, cap, "%s&mode=%s", base, mode)
                           : snprintf(dst, cap, "%s?mode=%s", base, mode);
    return n > 0 && (size_t)n < cap;
}

/** 与分片 init 相同：POST /upload?mode=daily 空 body（勿用 /upload/daily 子路径）。 */
static bool build_daily_url(char *dst, size_t cap)
{
    if (dst == nullptr || cap == 0) {
        return false;
    }
    const char *base = SOTI_R2_WORKER_URL;
    const char *q = strchr(base, '?');
    int n = (q != nullptr) ? snprintf(dst, cap, "%s&mode=daily", base) : snprintf(dst, cap, "%s?mode=daily", base);
    return n > 0 && (size_t)n < cap;
}

/** 分片 URL：?st=init&total= / ?st=part&sid=&off= / ?st=commit&sid=（body 可为空或非 JSON，避免 CDN 按 JPEG 魔数拦 JSON）。 */
static bool url_seg_init(char *dst, size_t cap, const char *base, unsigned total)
{
    if (dst == nullptr || cap == 0 || base == nullptr) {
        return false;
    }
    const char *q = strchr(base, '?');
    int n = (q != nullptr) ? snprintf(dst, cap, "%s&st=init&total=%u", base, total)
                           : snprintf(dst, cap, "%s?st=init&total=%u", base, total);
    return n > 0 && (size_t)n < cap;
}

static bool url_seg_part(char *dst, size_t cap, const char *base, const char *sid, unsigned off)
{
    if (dst == nullptr || cap == 0 || base == nullptr || sid == nullptr) {
        return false;
    }
    const char *q = strchr(base, '?');
    int n = (q != nullptr) ? snprintf(dst, cap, "%s&st=part&sid=%s&off=%u", base, sid, off)
                           : snprintf(dst, cap, "%s?st=part&sid=%s&off=%u", base, sid, off);
    return n > 0 && (size_t)n < cap;
}

static bool url_seg_commit(char *dst, size_t cap, const char *base, const char *sid)
{
    if (dst == nullptr || cap == 0 || base == nullptr || sid == nullptr) {
        return false;
    }
    const char *q = strchr(base, '?');
    int n = (q != nullptr) ? snprintf(dst, cap, "%s&st=commit&sid=%s", base, sid)
                           : snprintf(dst, cap, "%s?st=commit&sid=%s", base, sid);
    return n > 0 && (size_t)n < cap;
}

/** 分片模式与整包 POST 共用同一 URL（末尾 /upload），避免 CDN 仅放行 /upload 时子路径返回 403。 */
static bool worker_url_allows_segmented_upload(void)
{
    const char *u = SOTI_R2_WORKER_URL;
    const char *suffix = "/upload";
    size_t lu = strlen(u);
    size_t ls = strlen(suffix);
    if (lu < ls || strcmp(u + lu - ls, suffix) != 0) {
        ESP_LOGW(TAG, "segmented path needs SOTI_R2_WORKER_URL ending with /upload");
        return false;
    }
    return true;
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

/** 仅 https URL 启用证书包；明文 http 若仍设 crt_bundle，部分 IDF 上 fetch_headers 会失败但 status 已为 200。 */
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

static esp_err_t parse_json_sid(const char *json, char *sid, size_t sid_sz)
{
    cJSON *root = cJSON_Parse(json);
    if (root == nullptr) {
        return ESP_FAIL;
    }
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "sid");
    if (!cJSON_IsString(s) || s->valuestring == nullptr) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    copy_trunc_utf8(sid, sid_sz, s->valuestring);
    cJSON_Delete(root);
    return sid[0] != '\0' ? ESP_OK : ESP_FAIL;
}

static esp_err_t read_http_body_text(esp_http_client_handle_t client, char *buf, size_t cap, size_t *out_len)
{
    size_t total = 0;
    while (total + 1 < cap) {
        int r = esp_http_client_read(client, buf + total, (int)(cap - 1 - total));
        if (r < 0) {
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    if (out_len != nullptr) {
        *out_len = total;
    }
    return ESP_OK;
}

static esp_err_t parse_worker_response_json(
    const char *body, char *answer_out, size_t answer_out_sz, soti_print_sections_t *print_out)
{
    if (print_out != nullptr) {
        soti_print_sections_clear(print_out);
    }
    if (answer_out != nullptr && answer_out_sz > 0) {
        answer_out[0] = '\0';
    }
    if (answer_out == nullptr || answer_out_sz == 0) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse(body);
    if (root != nullptr) {
        const cJSON *ans = cJSON_GetObjectItemCaseSensitive(root, "answer");
        const cJSON *err_item = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsString(ans) && ans->valuestring != nullptr) {
            copy_trunc_utf8(answer_out, answer_out_sz, ans->valuestring);
        } else if (cJSON_IsString(err_item) && err_item->valuestring != nullptr) {
            copy_trunc_utf8(answer_out, answer_out_sz, err_item->valuestring);
        } else {
            copy_trunc_utf8(answer_out, answer_out_sz, body);
        }
        if (print_out != nullptr) {
            const cJSON *print_obj = cJSON_GetObjectItemCaseSensitive(root, "print");
            soti_print_sections_from_json(print_obj, print_out);
        }
        cJSON_Delete(root);
    } else {
        copy_trunc_utf8(answer_out, answer_out_sz, body);
    }
    return ESP_OK;
}

/** 多 TCP 短连接上传（init / part×N / commit），规避 ESP-Hosted SDIO 单连接 ~16KB 级卡死。 */
static esp_err_t soti_upload_jpeg_segmented(
    const uint8_t *jpeg,
    size_t jpeg_len,
    char *answer_out,
    size_t answer_out_sz,
    const char *mode,
    soti_print_sections_t *print_out)
{
    char upload_url[k_upload_url_cap];
    if (!build_worker_url_with_mode(upload_url, sizeof(upload_url), mode)) {
        return ESP_FAIL;
    }
    char url_seg[320];
    char sid[48];
    char resp[512];

    wifi_ps_type_t saved_ps = WIFI_PS_MIN_MODEM;
    (void)esp_wifi_get_ps(&saved_ps);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    /* --- init：query 带 total，body 为空（避免边缘层要求 body 以 JPEG 魔数开头）--- */
    if (!url_seg_init(url_seg, sizeof(url_seg), upload_url, (unsigned)jpeg_len)) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "segment init POST %s", url_seg);
    esp_http_client_config_t icfg = {};
    icfg.url = url_seg;
    icfg.method = HTTP_METHOD_POST;
    icfg.buffer_size = 2048;
    icfg.buffer_size_tx = 2048;
    icfg.timeout_ms = 120000;
    icfg.addr_type = HTTP_ADDR_TYPE_INET;
    http_client_cfg_tls_if_https(&icfg, url_seg);

    esp_http_client_handle_t c = esp_http_client_init(&icfg);
    if (c == nullptr) {
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    apply_bearer(c);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "segment init open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        (void)esp_wifi_set_ps(saved_ps);
        return err;
    }
    err = esp_http_client_fetch_headers(c);
    int st = esp_http_client_get_status_code(c);
    size_t rlen = 0;
    if (err != ESP_OK && st != 200) {
        ESP_LOGE(TAG, "segment init fetch_headers failed: %s status=%d", esp_err_to_name(err), st);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    if (err != ESP_OK && st == 200) {
        ESP_LOGW(TAG, "segment init fetch_headers %s but status=200, continue read", esp_err_to_name(err));
    }
    if (read_http_body_text(c, resp, sizeof(resp), &rlen) != ESP_OK) {
        ESP_LOGE(TAG, "segment init read body failed status=%d", st);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (st != 200 || parse_json_sid(resp, sid, sizeof(sid)) != ESP_OK) {
        ESP_LOGE(TAG, "segment init HTTP %d body=%s", st, resp);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "segment sid=%s total=%u parts=%u (one photo, %u bytes/part)",
             sid,
             (unsigned)jpeg_len,
             (unsigned)((jpeg_len + (size_t)k_segment_part_bytes - 1) / (size_t)k_segment_part_bytes),
             (unsigned)k_segment_part_bytes);

    /* --- parts --- */
    for (size_t off = 0; off < jpeg_len; off += (size_t)k_segment_part_bytes) {
        size_t part = jpeg_len - off;
        if (part > (size_t)k_segment_part_bytes) {
            part = (size_t)k_segment_part_bytes;
        }
        if (!url_seg_part(url_seg, sizeof(url_seg), upload_url, sid, (unsigned)off)) {
            (void)esp_wifi_set_ps(saved_ps);
            return ESP_FAIL;
        }
        esp_http_client_config_t pcfg = {};
        pcfg.url = url_seg;
        pcfg.method = HTTP_METHOD_POST;
        pcfg.buffer_size = 2048;
        pcfg.buffer_size_tx = 2048;
        pcfg.timeout_ms = 120000;
        pcfg.addr_type = HTTP_ADDR_TYPE_INET;
        http_client_cfg_tls_if_https(&pcfg, url_seg);

        esp_http_client_handle_t p = esp_http_client_init(&pcfg);
        if (p == nullptr) {
            (void)esp_wifi_set_ps(saved_ps);
            return ESP_FAIL;
        }
        apply_bearer(p);
        esp_http_client_set_header(p, "Accept-Encoding", "identity");
        esp_http_client_set_header(p, "Content-Type", "image/jpeg");
        err = esp_http_client_open(p, (int)part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "segment part open off=%u failed: %s", (unsigned)off, esp_err_to_name(err));
            esp_http_client_cleanup(p);
            (void)esp_wifi_set_ps(saved_ps);
            return err;
        }
        int written = 0;
        while (written < (int)part) {
            int chunk = (int)part - written;
            if (chunk > k_body_write_chunk) {
                chunk = k_body_write_chunk;
            }
            int w = esp_http_client_write(p, (const char *)(jpeg + off + (size_t)written), chunk);
            if (w <= 0) {
                ESP_LOGE(TAG, "segment part write off=%u written=%d w=%d", (unsigned)off, written, w);
                esp_http_client_close(p);
                esp_http_client_cleanup(p);
                (void)esp_wifi_set_ps(saved_ps);
                return ESP_FAIL;
            }
            written += w;
            vTaskDelay(pdMS_TO_TICKS(8));
            taskYIELD();
        }
        err = esp_http_client_fetch_headers(p);
        st = esp_http_client_get_status_code(p);
        if (err != ESP_OK && st == 200) {
            ESP_LOGW(TAG, "segment part fetch_headers off=%u: %s (status=200, continue)", (unsigned)off, esp_err_to_name(err));
        }
        /* drain small body */
        char tmp[256];
        esp_err_t rd = read_http_body_text(p, tmp, sizeof(tmp), nullptr);
        esp_http_client_close(p);
        esp_http_client_cleanup(p);
        if (st != 200 || rd != ESP_OK) {
            ESP_LOGE(
                TAG,
                "segment part off=%u st=%d fetch_headers=%s read=%s",
                (unsigned)off,
                st,
                esp_err_to_name(err),
                esp_err_to_name(rd));
            (void)esp_wifi_set_ps(saved_ps);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "segment part ok off=%u len=%u", (unsigned)off, (unsigned)part);
        vTaskDelay(pdMS_TO_TICKS(k_segment_part_gap_ms));
        taskYIELD();
    }

    /* --- commit：query 带 sid，body 为空 --- */
    if (!url_seg_commit(url_seg, sizeof(url_seg), upload_url, sid)) {
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    esp_http_client_config_t ccfg = {};
    ccfg.url = url_seg;
    ccfg.method = HTTP_METHOD_POST;
    ccfg.buffer_size = 8192;
    ccfg.buffer_size_tx = 8192;
    ccfg.timeout_ms = 360000;
    ccfg.addr_type = HTTP_ADDR_TYPE_INET;
    http_client_cfg_tls_if_https(&ccfg, url_seg);

    c = esp_http_client_init(&ccfg);
    if (c == nullptr) {
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    apply_bearer(c);
    esp_http_client_set_header(c, "Content-Type", "image/jpeg");
    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(c);
        (void)esp_wifi_set_ps(saved_ps);
        return err;
    }
    err = esp_http_client_fetch_headers(c);
    st = esp_http_client_get_status_code(c);
    char *body = alloc_response_body(k_resp_body_cap);
    if (body == nullptr) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "segment commit fetch_headers: %s status=%d (read body anyway)", esp_err_to_name(err), st);
    }
    if (read_http_body_text(c, body, k_resp_body_cap, nullptr) != ESP_OK) {
        heap_caps_free(body);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        (void)esp_wifi_set_ps(saved_ps);
        ESP_LOGE(TAG, "segment commit read body failed status=%d", st);
        return ESP_FAIL;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    (void)esp_wifi_set_ps(saved_ps);

    ESP_LOGI(TAG, "segment commit HTTP status=%d", st);
    if (st != 200) {
        parse_worker_response_json(body, answer_out, answer_out_sz, print_out);
        heap_caps_free(body);
        return ESP_FAIL;
    }
    parse_worker_response_json(body, answer_out, answer_out_sz, print_out);
    heap_caps_free(body);
    ESP_LOGI(TAG, "Upload OK segmented (%u bytes JPEG)", (unsigned)jpeg_len);
    return ESP_OK;
}

extern "C" esp_err_t soti_fetch_daily_line(char *answer_out, size_t answer_out_sz, soti_print_sections_t *print_out)
{
    if (answer_out != nullptr && answer_out_sz > 0) {
        answer_out[0] = '\0';
    }
    char daily_url[k_upload_url_cap];
    if (!build_daily_url(daily_url, sizeof(daily_url))) {
        ESP_LOGE(TAG, "daily: build URL failed (check SOTI_R2_WORKER_URL)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "daily POST %s (same path as upload init)", daily_url);

    wifi_ps_type_t saved_ps = WIFI_PS_MIN_MODEM;
    (void)esp_wifi_get_ps(&saved_ps);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    esp_http_client_config_t cfg = {};
    cfg.url = daily_url;
    cfg.method = HTTP_METHOD_POST;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.timeout_ms = 120000;
    cfg.addr_type = HTTP_ADDR_TYPE_INET;
    http_client_cfg_tls_if_https(&cfg, daily_url);

    char *body = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (body == nullptr) {
        body = (char *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (body == nullptr) {
        ESP_LOGE(TAG, "daily: body buffer OOM");
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        heap_caps_free(body);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    apply_bearer(client);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "daily open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        heap_caps_free(body);
        (void)esp_wifi_set_ps(saved_ps);
        return err;
    }
    err = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK && status != 200) {
        ESP_LOGE(TAG, "daily fetch_headers failed: %s status=%d", esp_err_to_name(err), status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        heap_caps_free(body);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    if (read_http_body_text(client, body, 4096, nullptr) != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        heap_caps_free(body);
        (void)esp_wifi_set_ps(saved_ps);
        return ESP_FAIL;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    (void)esp_wifi_set_ps(saved_ps);

    ESP_LOGI(TAG, "daily HTTP status=%d body_len=%u", status, (unsigned)strlen(body));
    if (status != 200) {
        parse_worker_response_json(body, answer_out, answer_out_sz, print_out);
        heap_caps_free(body);
        return ESP_FAIL;
    }
    parse_worker_response_json(body, answer_out, answer_out_sz, print_out);
    heap_caps_free(body);
    return ESP_OK;
}

extern "C" esp_err_t soti_upload_jpeg_to_worker(
    const uint8_t *jpeg,
    size_t jpeg_len,
    char *answer_out,
    size_t answer_out_sz,
    const char *mode,
    soti_print_sections_t *print_out)
{
    if (jpeg == nullptr || jpeg_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (answer_out != nullptr && answer_out_sz > 0) {
        answer_out[0] = '\0';
    }

    ESP_LOGI(
        TAG,
        "POST JPEG mode=%s len=%u sig=%02x%02x",
        (mode != nullptr && mode[0] != '\0') ? mode : "solve",
        (unsigned)jpeg_len,
        jpeg_len >= 1 ? (unsigned)jpeg[0] : 0u,
        jpeg_len >= 2 ? (unsigned)jpeg[1] : 0u);

    if (jpeg_len > (size_t)SOTI_UPLOAD_SEGMENT_THRESHOLD && worker_url_allows_segmented_upload()) {
        ESP_LOGI(TAG, "use segmented upload (threshold=%d)", SOTI_UPLOAD_SEGMENT_THRESHOLD);
        return soti_upload_jpeg_segmented(jpeg, jpeg_len, answer_out, answer_out_sz, mode, print_out);
    }

    char upload_url[k_upload_url_cap];
    if (!build_worker_url_with_mode(upload_url, sizeof(upload_url), mode)) {
        return ESP_FAIL;
    }

    esp_http_client_config_t cfg = {};
    cfg.url = upload_url;
    cfg.method = HTTP_METHOD_POST;
    cfg.buffer_size = 8192;
    cfg.buffer_size_tx = 8192;
    cfg.timeout_ms = 360000;
    cfg.addr_type = HTTP_ADDR_TYPE_INET;
    http_client_cfg_tls_if_https(&cfg, upload_url);

    constexpr int k_open_retries = 4;
    esp_http_client_handle_t client = nullptr;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < k_open_retries; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "http open retry %d/%d after %s", attempt + 1, k_open_retries, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(600));
        }

        client = esp_http_client_init(&cfg);
        if (client == nullptr) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "http client init failed");
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "image/jpeg");
        apply_bearer(client);

        err = esp_http_client_open(client, (int)jpeg_len);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        client = nullptr;
    }

    if (client == nullptr) {
        return err;
    }

    ESP_LOGI(TAG, "HTTP open OK, sending body (chunk=%d stage=%d)", k_body_write_chunk, k_stage_bytes);

    uint8_t *stage = (uint8_t *)heap_caps_malloc(k_stage_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stage == nullptr) {
        ESP_LOGW(TAG, "no internal stage buf; send from JPEG buf directly");
    }

    wifi_ps_type_t saved_ps = WIFI_PS_MIN_MODEM;
    (void)esp_wifi_get_ps(&saved_ps);
    (void)esp_wifi_set_ps(WIFI_PS_NONE);

    int to_write = (int)jpeg_len;
    int written = 0;
    while (written < to_write) {
        int chunk = to_write - written;
        int max_chunk = k_body_write_chunk;
        if (written >= 12288) {
            max_chunk = 256;
        }
        if (chunk > max_chunk) {
            chunk = max_chunk;
        }
        const int before = written;
        const char *out_ptr = (const char *)(jpeg + written);
        if (stage != nullptr) {
            memcpy(stage, jpeg + (size_t)written, (size_t)chunk);
            out_ptr = (const char *)stage;
        }
        if ((written % 4096) == 0) {
            ESP_LOGI(TAG, "upload write try off=%d len=%d", written, chunk);
        }
        if (written == 12288 || written == 16384) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        if (written > 0) {
            int ms = 5;
            if (written >= 65536) {
                ms = 20;
            } else if (written >= 32768) {
                ms = 15;
            } else if (written >= 16384) {
                ms = 28;
            } else if (written >= 8192) {
                ms = 8;
            }
            vTaskDelay(pdMS_TO_TICKS(ms));
        }
        int w = esp_http_client_write(client, out_ptr, chunk);
        if (w <= 0) {
            ESP_LOGE(TAG, "http write failed at %d/%d (peer RST? check Nginx/burst)", written, to_write);
            (void)esp_wifi_set_ps(saved_ps);
            heap_caps_free(stage);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        written += w;
        if (before == 0 || written == to_write || (written / 32768) != (before / 32768)) {
            ESP_LOGI(TAG, "upload wrote %d/%d bytes", written, to_write);
        }
        if (written < to_write && (written % 4096) == 0) {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
        taskYIELD();
    }

    heap_caps_free(stage);

    (void)esp_wifi_set_ps(saved_ps);

    ESP_LOGI(TAG, "HTTP body sent (%d bytes), waiting for response headers", written);
    esp_err_t fh = esp_http_client_fetch_headers(client);
    if (fh != ESP_OK) {
        ESP_LOGE(TAG, "fetch_headers failed: %s", esp_err_to_name(fh));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return fh;
    }
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status=%d (response headers received)", status);

    char *body = alloc_response_body(k_resp_body_cap);
    if (body == nullptr) {
        ESP_LOGE(TAG, "body malloc failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total + 1 < k_resp_body_cap) {
        int r = esp_http_client_read(client, body + total, (int)(k_resp_body_cap - 1 - total));
        if (r < 0) {
            ESP_LOGE(TAG, "http read failed");
            heap_caps_free(body);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (r == 0) {
            break;
        }
        total += (size_t)r;
    }
    body[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200) {
        ESP_LOGE(TAG, "Worker HTTP status %d", status);
        parse_worker_response_json(body, answer_out, answer_out_sz, print_out);
        heap_caps_free(body);
        return ESP_FAIL;
    }

    parse_worker_response_json(body, answer_out, answer_out_sz, print_out);
    heap_caps_free(body);
    ESP_LOGI(TAG, "Upload OK (%u bytes JPEG)", (unsigned)jpeg_len);
    return ESP_OK;
}
