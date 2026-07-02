#include "xiaozhi_service.h"
#include "xiaozhi_config.h"
#include "parent_net_gate.h"
#include "power_manager.h"
#include "bsp_board_extra.h"
#include "bsp/esp-bsp.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"
#include "esp_http_client.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "opus_encoder.h"
#include "opus_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/i2s_std.h"
#include <cstring>
#include <vector>
#include <memory>
static const char *TAG = "xz_svc";
static xz_svc_cb_t s_cb;
static void *s_ctx;
static esp_xiaozhi_chat_handle_t s_chat;
static TaskHandle_t s_init_task;
static TaskHandle_t s_rec_task;
static TaskHandle_t s_play_task;
static TaskHandle_t s_watch_task;
static QueueHandle_t s_play_q;
typedef struct { uint16_t len; uint8_t d[XZ_OPUS_PKT_MAX]; } xz_play_pkt_t;
static SemaphoreHandle_t s_mtx;
static volatile bool s_run, s_ptt, s_ch_open, s_play_fs;
static volatile bool s_wait_reply;
static volatile int64_t s_wait_since_us;
static volatile uint32_t s_ptt_seq, s_ptt_frames, s_ptt_send_err, s_ptt_read_err;
static volatile uint32_t s_ptt_peak_l, s_ptt_peak_r;
static std::unique_ptr<OpusEncoderWrapper> s_enc;
static std::unique_ptr<OpusDecoderWrapper> s_dec;
static const esp_xiaozhi_chat_audio_t s_audio = { XZ_AUDIO_FMT, XZ_SR, XZ_AUDIO_CH, XZ_FRAME_MS };
static void emit(xz_svc_state_t st, const char *line, const char *txt, bool user)
{
    if (!s_cb) return;
    xz_svc_evt_t e{};
    e.state = st;
    e.user = user;
    if (line) snprintf(e.line, sizeof(e.line), "%s", line);
    if (txt) snprintf(e.text, sizeof(e.text), "%s", txt);
    s_cb(&e, s_ctx);
}
static void emit_err(const char *prefix, esp_err_t err)
{
    char line[160];
    if (err == ESP_ERR_HTTP_CONNECT) {
        snprintf(line, sizeof(line), "%s: %s(TLS/网络)", prefix, esp_err_to_name(err));
    } else if (err == ESP_ERR_NO_MEM) {
        snprintf(line, sizeof(line), "%s: %s(内存不足)", prefix, esp_err_to_name(err));
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(line, sizeof(line), "%s: %s(MCP未初始化)", prefix, esp_err_to_name(err));
    } else {
        snprintf(line, sizeof(line), "%s: %s", prefix, esp_err_to_name(err));
    }
    ESP_LOGE(TAG, "%s", line);
    emit(XZ_SVC_ERROR, line, nullptr, false);
}
static void heap_log(const char *st)
{
    ESP_LOGW(TAG, "[%s] heap int free=%u min=%u", st,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
static esp_mcp_value_t mcp_vol(const esp_mcp_property_list_t *p)
{
    int v = esp_mcp_property_list_get_property_int(p, "volume");
    if (v < 0 || v > 100) return esp_mcp_value_create_bool(false);
    int set = 0;
    bsp_extra_codec_volume_set(v, &set);
    return esp_mcp_value_create_bool(true);
}
static esp_mcp_value_t mcp_bri(const esp_mcp_property_list_t *p)
{
    int b = esp_mcp_property_list_get_property_int(p, "brightness");
    if (b < 0 || b > 100) return esp_mcp_value_create_bool(false);
    bsp_display_brightness_set(b);
    power_manager_set_backlight_percent(b);
    return esp_mcp_value_create_bool(true);
}
static esp_mcp_t *mk_mcp(void)
{
    esp_mcp_t *m = nullptr;
    if (esp_mcp_create(&m) != ESP_OK) return nullptr;
    esp_mcp_tool_t *t1 = esp_mcp_tool_create("self.audio_speaker.set_volume", "音量0-100", mcp_vol);
    esp_mcp_tool_add_property(t1, esp_mcp_property_create_with_range("volume", 0, 100));
    esp_mcp_add_tool(m, t1);
    esp_mcp_tool_t *t2 = esp_mcp_tool_create("self.screen.set_brightness", "亮度0-100", mcp_bri);
    esp_mcp_tool_add_property(t2, esp_mcp_property_create_with_range("brightness", 0, 100));
    esp_mcp_add_tool(m, t2);
    return m;
}
static void play_task(void *)
{
    static int16_t st[XZ_FRAME_SAMPLES * 2];
    xz_play_pkt_t pkt;
    ESP_LOGW(TAG, "play task started stack=%u", (unsigned)XZ_PLAY_TASK_STACK);
    while (s_run) {
        if (xQueueReceive(s_play_q, &pkt, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        if (!s_dec || !pkt.len) continue;
        std::vector<uint8_t> op(pkt.d, pkt.d + pkt.len);
        std::vector<int16_t> pcm;
        if (!s_dec->Decode(std::move(op), pcm) || pcm.empty()) continue;
        size_t n = pcm.size() > XZ_FRAME_SAMPLES ? XZ_FRAME_SAMPLES : pcm.size();
        for (size_t i = 0; i < n; i++) { st[i * 2] = pcm[i]; st[i * 2 + 1] = pcm[i]; }
        size_t w = 0;
        bsp_extra_i2s_write(st, n * 2 * sizeof(int16_t), &w, 100);
    }
    s_play_task = nullptr;
    vTaskDeleteWithCaps(NULL);
}
static void on_audio(const uint8_t *d, int len, void *)
{
    if (!d || len <= 0 || len > XZ_OPUS_PKT_MAX || !s_play_q) return;
    xz_play_pkt_t p{}; p.len = (uint16_t)len; memcpy(p.d, d, len);
    xQueueSend(s_play_q, &p, 0);
}
static void on_evt(esp_xiaozhi_chat_event_t ev, void *data, void *)
{
    ESP_LOGW(TAG, "chat event=%d", (int)ev);
    s_wait_since_us = esp_timer_get_time();
    switch (ev) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        auto *t = static_cast<esp_xiaozhi_chat_text_data_t *>(data);
        if (t && t->text) {
            ESP_LOGW(TAG, "chat text role=%d len=%u", (int)t->role, (unsigned)strlen(t->text));
            if (t->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER) emit(XZ_SVC_THINKING, "思考中…", t->text, true);
            else {
                s_wait_reply = false;
                emit(XZ_SVC_SPEAKING, nullptr, t->text, false);
            }
        }
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TTS_STATE: {
        auto *ts = static_cast<esp_xiaozhi_chat_tts_state_t *>(data);
        if (!ts) break;
        ESP_LOGW(TAG, "tts state=%d text_len=%u", (int)ts->state, (unsigned)(ts->text ? strlen(ts->text) : 0));
        s_wait_reply = false;
        if (ts->state == ESP_XIAOZHI_CHAT_TTS_STATE_START) emit(XZ_SVC_SPEAKING, "播放中…", nullptr, false);
        else if (ts->state == ESP_XIAOZHI_CHAT_TTS_STATE_STOP) emit(XZ_SVC_READY, "待命，按住说话", nullptr, false);
        break;
    }
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        auto *er = static_cast<esp_xiaozhi_chat_error_info_t *>(data);
        char line[160];
        snprintf(line, sizeof(line), "协议错误: %s %s", er && er->source ? er->source : "?", esp_err_to_name(er ? er->code : ESP_FAIL));
        s_wait_reply = false;
        emit(XZ_SVC_ERROR, line, nullptr, false);
        break;
    }
    default: break;
    }
}
static void rec_task(void *)
{
    static int16_t mono_buf[XZ_FRAME_SAMPLES], st_buf[XZ_FRAME_SAMPLES * 2];
    ESP_LOGW(TAG, "rec task started stack=%u caps=SPIRAM", (unsigned)XZ_REC_TASK_STACK);
    unsigned sent = 0, send_err = 0, read_err = 0;
    while (s_run) {
        if (!s_ptt) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        size_t br = 0;
        if (bsp_extra_i2s_read(st_buf, XZ_ST_PCM_BYTES, &br, 50) != ESP_OK || br < XZ_ST_PCM_BYTES) {
            s_ptt_read_err++;
            if ((++read_err % 30) == 1) ESP_LOGW(TAG, "i2s_read fail br=%u", (unsigned)br);
            continue;
        }
        uint32_t peak_l = 0, peak_r = 0;
        uint64_t sum_l = 0, sum_r = 0;
        for (size_t i = 0; i < XZ_FRAME_SAMPLES; i++) {
            int16_t l = st_buf[i * 2], r = st_buf[i * 2 + 1];
            uint32_t al = (l < 0) ? (uint32_t)(-l) : (uint32_t)l;
            uint32_t ar = (r < 0) ? (uint32_t)(-r) : (uint32_t)r;
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
            sum_l += al;
            sum_r += ar;
        }
        const bool use_r = peak_r > peak_l;
        for (size_t i = 0; i < XZ_FRAME_SAMPLES; i++) mono_buf[i] = use_r ? st_buf[i * 2 + 1] : st_buf[i * 2];
        if (peak_l > s_ptt_peak_l) s_ptt_peak_l = peak_l;
        if (peak_r > s_ptt_peak_r) s_ptt_peak_r = peak_r;
        std::vector<int16_t> mono(mono_buf, mono_buf + XZ_FRAME_SAMPLES);
        std::vector<uint8_t> opus;
        if (!s_enc || !s_enc->Encode(std::move(mono), opus) || opus.empty()) continue;
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        esp_err_t se = s_chat ? esp_xiaozhi_chat_send_audio_data(s_chat, (const char *)opus.data(), opus.size()) : ESP_ERR_INVALID_STATE;
        xSemaphoreGive(s_mtx);
        if (se == ESP_OK) {
            s_ptt_frames++;
            if ((++sent % 10) == 0) {
                ESP_LOGW(TAG, "audio sent seq=%u frames=%u opus=%u avg_l=%u avg_r=%u peak_l=%u peak_r=%u",
                         (unsigned)s_ptt_seq, sent, (unsigned)opus.size(),
                         (unsigned)(sum_l / XZ_FRAME_SAMPLES), (unsigned)(sum_r / XZ_FRAME_SAMPLES),
                         (unsigned)peak_l, (unsigned)peak_r);
            }
        } else {
            s_ptt_send_err++;
            if ((++send_err % 20) == 1) ESP_LOGW(TAG, "send_audio_data: %s", esp_err_to_name(se));
        }
    }
    s_rec_task = nullptr;
    vTaskDeleteWithCaps(NULL);
}
static void watch_task(void *)
{
    ESP_LOGW(TAG, "watch task started");
    while (s_run) {
        if (s_wait_reply && s_wait_since_us > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - s_wait_since_us) / 1000;
            if (elapsed_ms > XZ_REPLY_TIMEOUT_MS) {
                ESP_LOGW(TAG, "reply timeout after %lld ms seq=%u frames=%u read_err=%u send_err=%u peak_l=%u peak_r=%u",
                         (long long)elapsed_ms, (unsigned)s_ptt_seq, (unsigned)s_ptt_frames,
                         (unsigned)s_ptt_read_err, (unsigned)s_ptt_send_err,
                         (unsigned)s_ptt_peak_l, (unsigned)s_ptt_peak_r);
                s_wait_reply = false;
                emit(XZ_SVC_READY, "无回复超时，请重试", nullptr, false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_watch_task = nullptr;
    vTaskDelete(nullptr);
}
static bool mk_opus(void)
{
    s_enc=std::make_unique<OpusEncoderWrapper>(XZ_SR,1,XZ_FRAME_MS);
    s_enc->SetComplexity(0);
    s_enc->SetDtx(false);
    s_dec=std::make_unique<OpusDecoderWrapper>(XZ_SR,1,XZ_FRAME_MS);
    if(!s_enc->IsConfigured()||!s_dec->IsConfigured()){s_enc.reset();s_dec.reset();return false;}
    return true;
}
static void init_task(void *)
{
    if (!parent_net_sta_has_ip()) {
        emit(XZ_SVC_NEED_WIFI, "请先在设置中连接 WiFi", nullptr, false);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    emit(XZ_SVC_CONNECTING, "连接小智平台…", nullptr, false);
    ESP_LOGW(TAG, "wait WiFi/SDIO settle %u ms before xiaozhi TLS", (unsigned)XZ_START_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(XZ_START_SETTLE_MS));
    if (!s_run) {
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    heap_log("start");
    esp_xiaozhi_chat_info_t info{};
    esp_err_t err = esp_xiaozhi_chat_get_info(&info);
    if (err != ESP_OK) {
        s_wait_reply = false;
        emit_err("OTA/信息获取失败", err);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    heap_log("get_info");
    if (info.has_activation_code) {
        char line[160];
        snprintf(line, sizeof(line), "请到 xiaozhi.me 激活  设备:%s  码:%s", info.serial_number ? info.serial_number : "?", info.activation_code ? info.activation_code : "?");
        emit(XZ_SVC_NEED_ACTIVATE, line, nullptr, false);
        esp_xiaozhi_chat_free_info(&info);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    esp_mcp_t *mcp = mk_mcp();
    if (!mcp) {
        emit_err("MCP初始化失败", ESP_ERR_NO_MEM);
        esp_xiaozhi_chat_free_info(&info);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    heap_log("mk_mcp");
    if (!mk_opus()) {
        esp_mcp_destroy(mcp);
        esp_xiaozhi_chat_free_info(&info);
        emit_err("OPUS初始化失败", ESP_ERR_NO_MEM);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    heap_log("mk_opus");
    esp_xiaozhi_chat_config_t cfg = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    cfg.audio_callback = on_audio;
    cfg.event_callback = on_evt;
    cfg.mcp_engine = mcp;
    cfg.owns_mcp_engine = true;
    cfg.has_mqtt_config = info.has_mqtt_config;
    cfg.has_websocket_config = info.has_websocket_config;
    esp_xiaozhi_chat_handle_t hd = 0;
    err = esp_xiaozhi_chat_init(&cfg, &hd);
    esp_xiaozhi_chat_free_info(&info);
    if (err != ESP_OK) {
        if (hd) esp_xiaozhi_chat_deinit(hd);
        else esp_mcp_destroy(mcp);
        emit_err("小智初始化失败", err);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    mcp = nullptr;
    heap_log("chat_init");
    err = esp_xiaozhi_chat_start(hd);
    if (err != ESP_OK) {
        esp_xiaozhi_chat_deinit(hd);
        emit_err("小智连接失败", err);
        s_run = false;
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    heap_log("chat_start");
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_chat = hd;
    xSemaphoreGive(s_mtx);
    err = bsp_extra_codec_set_fs(XZ_SR, CODEC_DEFAULT_BIT_WIDTH, I2S_SLOT_MODE_STEREO);
    ESP_LOGW(TAG, "codec set fs for xiaozhi ret=%s", esp_err_to_name(err));
    if (err == ESP_OK) s_play_fs = true;
    if (err != ESP_OK) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_chat) {
            esp_xiaozhi_chat_stop(s_chat);
            esp_xiaozhi_chat_deinit(s_chat);
            s_chat = 0;
        }
        xSemaphoreGive(s_mtx);
        s_enc.reset();
        s_dec.reset();
        s_ch_open = false;
        s_play_fs = false;
        s_run = false;
        emit_err("codec set fs failed", err);
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    BaseType_t rec_ok = xTaskCreateWithCaps(rec_task, "xz_rec", XZ_REC_TASK_STACK, nullptr, XZ_REC_PRIO, &s_rec_task,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "rec task create ret=%ld handle=%p heap int free=%u psram free=%u",
             (long)rec_ok, s_rec_task,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (rec_ok != pdPASS || !s_rec_task) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (s_chat) {
            esp_xiaozhi_chat_stop(s_chat);
            esp_xiaozhi_chat_deinit(s_chat);
            s_chat = 0;
        }
        xSemaphoreGive(s_mtx);
        s_enc.reset();
        s_dec.reset();
        s_ch_open = false;
        s_play_fs = false;
        s_run = false;
        emit_err("rec task start failed", ESP_ERR_NO_MEM);
        s_init_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    emit(XZ_SVC_READY, "已连接，按住说话", nullptr, false);
    s_init_task = nullptr;
    vTaskDelete(nullptr);
}
extern "C" esp_err_t xz_svc_start(xz_svc_cb_t cb, void *ctx)
{
    if (s_run) return ESP_ERR_INVALID_STATE;
    if (s_init_task || s_rec_task || s_play_task || s_watch_task) {
        ESP_LOGW(TAG, "start blocked by stale task init=%p rec=%p play=%p watch=%p", s_init_task, s_rec_task, s_play_task, s_watch_task);
        return ESP_ERR_INVALID_STATE;
    }
    s_cb = cb;
    s_ctx = ctx;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_play_q) s_play_q = xQueueCreate(XZ_PLAY_Q_DEPTH, sizeof(xz_play_pkt_t));
    if (!s_play_q) { emit_err("play queue failed", ESP_ERR_NO_MEM); return ESP_ERR_NO_MEM; }
    s_run = true;
    s_ptt = false;
    s_ch_open = false;
    s_play_fs = false;
    s_wait_reply = false;
    s_wait_since_us = 0;
    BaseType_t play_ok = xTaskCreateWithCaps(play_task, "xz_play", XZ_PLAY_TASK_STACK, nullptr, XZ_PLAY_PRIO, &s_play_task,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "play task create ret=%ld handle=%p", (long)play_ok, s_play_task);
    if (play_ok != pdPASS || !s_play_task) {
        emit_err("play task start failed", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    BaseType_t watch_ok = xTaskCreate(watch_task, "xz_watch", XZ_WATCH_TASK_STACK, nullptr, XZ_WATCH_PRIO, &s_watch_task);
    ESP_LOGW(TAG, "watch task create ret=%ld handle=%p", (long)watch_ok, s_watch_task);
    if (watch_ok != pdPASS || !s_watch_task) {
        s_run = false;
        for (int i = 0; s_play_task && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_play_task) { vTaskDeleteWithCaps(s_play_task); s_play_task = nullptr; }
        emit_err("watch task start failed", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    BaseType_t init_ok = xTaskCreate(init_task, "xz_init", XZ_INIT_TASK_STACK, nullptr, XZ_INIT_PRIO, &s_init_task);
    ESP_LOGW(TAG, "init task create ret=%ld handle=%p", (long)init_ok, s_init_task);
    if (init_ok != pdPASS || !s_init_task) {
        s_run = false;
        for (int i = 0; s_play_task && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
        if (s_play_task) { vTaskDeleteWithCaps(s_play_task); s_play_task = nullptr; }
        while (s_watch_task) vTaskDelay(pdMS_TO_TICKS(10));
        emit_err("init task start failed", ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
extern "C" esp_err_t xz_svc_stop(void)
{
    if (!s_run) return ESP_OK;
    s_run = false;
    s_ptt = false;
    bsp_extra_codec_dev_stop();
    for (int i = 0; s_rec_task && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_rec_task) { ESP_LOGW(TAG, "rec task force delete"); vTaskDeleteWithCaps(s_rec_task); s_rec_task = nullptr; }
    for (int i = 0; s_play_task && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_play_task) { ESP_LOGW(TAG, "play task force delete"); vTaskDeleteWithCaps(s_play_task); s_play_task = nullptr; }
    if (s_play_q) { xQueueReset(s_play_q); }
    while (s_watch_task) vTaskDelay(pdMS_TO_TICKS(10));
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_chat) {
        esp_xiaozhi_chat_stop(s_chat);
        esp_xiaozhi_chat_deinit(s_chat);
        s_chat = 0;
    }
    xSemaphoreGive(s_mtx);
    s_enc.reset();
    s_dec.reset();
    s_ch_open = false;
    s_play_fs = false;
    s_wait_reply = false;
    s_wait_since_us = 0;
    bsp_extra_codec_dev_resume();
    emit(XZ_SVC_IDLE, nullptr, nullptr, false);
    return ESP_OK;
}
extern "C" esp_err_t xz_svc_ptt_down(void)
{
    if (!s_run || !s_chat || !s_enc || !s_rec_task) {
        ESP_LOGW(TAG, "ptt down invalid state run=%d chat=0x%lx enc=%p rec=%p",
                 (int)s_run, (unsigned long)s_chat, s_enc.get(), s_rec_task);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_ch_open) {
        ESP_LOGW(TAG, "open audio channel fmt=%s sr=%d ch=%d frame_ms=%d", XZ_AUDIO_FMT, XZ_SR, XZ_AUDIO_CH, XZ_FRAME_MS);
        err = esp_xiaozhi_chat_open_audio_channel(s_chat, &s_audio, nullptr, 0);
        if (err == ESP_OK) s_ch_open = true;
    }
    ESP_LOGW(TAG, "start listening seq=%u open_err=%s", (unsigned)(s_ptt_seq + 1), esp_err_to_name(err));
    if (err == ESP_OK) err = esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
    xSemaphoreGive(s_mtx);
    if (err != ESP_OK) return err;
    if (s_enc) s_enc->ResetState();
    s_ptt_seq++;
    s_ptt_frames = 0;
    s_ptt_send_err = 0;
    s_ptt_read_err = 0;
    s_ptt_peak_l = 0;
    s_ptt_peak_r = 0;
    s_wait_reply = false;
    s_wait_since_us = 0;
    s_ptt = true;
    emit(XZ_SVC_LISTENING, "聆听中…", nullptr, false);
    return ESP_OK;
}
extern "C" esp_err_t xz_svc_ptt_up(void)
{
    if (!s_run || !s_chat) return ESP_ERR_INVALID_STATE;
    s_ptt = false;
    ESP_LOGW(TAG, "stop listening seq=%u frames=%u read_err=%u send_err=%u peak_l=%u peak_r=%u",
             (unsigned)s_ptt_seq, (unsigned)s_ptt_frames, (unsigned)s_ptt_read_err,
             (unsigned)s_ptt_send_err, (unsigned)s_ptt_peak_l, (unsigned)s_ptt_peak_r);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t err = esp_xiaozhi_chat_send_stop_listening(s_chat);
    xSemaphoreGive(s_mtx);
    s_wait_reply = true;
    s_wait_since_us = esp_timer_get_time();
    if (err != ESP_OK) {
        s_wait_reply = false;
        emit_err("停止录音失败", err);
        emit(XZ_SVC_READY, "已连接，按住说话", nullptr, false);
        return err;
    }
    emit(XZ_SVC_THINKING, "思考中…", nullptr, false);
    return ESP_OK;
}
extern "C" bool xz_svc_running(void) { return s_run; }
