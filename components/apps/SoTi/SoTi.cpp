/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "SoTi.hpp"
#include "esp_err.h"
#include "bsp/esp-bsp.h"
#include "camera/Camera.hpp"
#include "esp_brookesia.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lv_font_ui_zh.h"
#include "soti_answer_format.hpp"
#include "soti_r2_upload.hpp"
#include "soti_print_sections.hpp"
#include "power_manager.h"
#include "parent_album_sync.h"
#include "parent_print_sync.h"
#include "parent_chat/parent_chat_api.hpp"
#include "soti_mode.h"
#include "soti_config.h"
#include "escpos_text_print.h"
#include "uart_escpos_printer.h"
#include "usb_escpos_printer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *TAG = "SoTi";

static constexpr uint32_t kMaxSdJpegBytes = 6 * 1024 * 1024;
static portMUX_TYPE s_upload_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_upload_worker_busy = false;

/** Set by armSdCardJpegUpload(), consumed once when SoTi run()/resume() opens. */
static char s_armed_sd_path[512];

LV_IMG_DECLARE(img_app_soti);

typedef struct {
    uint8_t *jpeg;
    size_t len;
    SoTi *app;
    char mode[16];
    soti_mode_t result_mode;
} soti_upload_ctx_t;

typedef struct {
    SoTi *app;
} soti_daily_ctx_t;

typedef struct {
    SoTi *app;
    char *text;
    soti_mode_t mode;
    soti_print_sections_t print;
} soti_async_pack_t;

typedef struct {
    SoTi *app;
    char msg[48];
} soti_print_status_pack_t;

typedef struct {
    SoTi *app;
    bool with_answer;
} soti_print_job_t;

typedef struct {
    SoTi *app;
    uint8_t *jpeg;
    size_t jpeg_len;
} soti_confirm_upload_t;

typedef struct {
    SoTi *app;
    uint8_t *jpeg;
    size_t len;
} soti_sd_ready_pack_t;

static char *strdup_fallback(const char *s)
{
    if (s == nullptr) {
        return nullptr;
    }
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p == nullptr) {
        return nullptr;
    }
    memcpy(p, s, n);
    return p;
}

static void soti_apply_zh_font_22(lv_obj_t *obj)
{
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(obj, &lv_font_ui_zh_22, LV_PART_MAIN | LV_STATE_DEFAULT);
    const uint32_t n = lv_obj_get_child_cnt(obj);
    for (uint32_t i = 0; i < n; i++) {
        soti_apply_zh_font_22(lv_obj_get_child(obj, i));
    }
}

static void soti_answer_lv_async_cb(void *ud)
{
    soti_async_pack_t *p = (soti_async_pack_t *)ud;
    if (p == nullptr) {
        return;
    }
    if (p->app != nullptr) {
        p->app->showLoadingUi(false);
        p->app->setPrintCache(p->mode, &p->print);
        p->app->showAnswerUi(p->text != nullptr ? p->text : "");
    }
    if (p->text != nullptr) {
        free(p->text);
    }
    free(p);
}

static void soti_print_status_lv_async_cb(void *ud)
{
    soti_print_status_pack_t *p = (soti_print_status_pack_t *)ud;
    if (p != nullptr && p->app != nullptr) {
        p->app->postPrintStatusAsync(p->msg);
    }
    free(p);
}

static void soti_sd_ready_lv_async_cb(void *ud)
{
    soti_sd_ready_pack_t *p = (soti_sd_ready_pack_t *)ud;
    if (p == nullptr) {
        return;
    }
    if (p->app != nullptr && p->jpeg != nullptr && p->len > 0) {
        p->app->deliverSdJpegForUpload(p->jpeg, p->len);
        p->jpeg = nullptr;
    } else if (p->jpeg != nullptr) {
        heap_caps_free(p->jpeg);
    }
    free(p);
}

static void soti_sd_consume_task(void *arg)
{
    SoTi *app = (SoTi *)arg;
    if (app == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    if (s_armed_sd_path[0] == '\0') {
        vTaskDelete(nullptr);
        return;
    }

    char path[sizeof(s_armed_sd_path)];
    memcpy(path, s_armed_sd_path, sizeof(path));
    s_armed_sd_path[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        ESP_LOGE(TAG, "SD JPEG open failed: %s", path);
        vTaskDelete(nullptr);
        return;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        vTaskDelete(nullptr);
        return;
    }
    long fsz = ftell(f);
    if (fsz <= 0 || (uint32_t)fsz > kMaxSdJpegBytes) {
        fclose(f);
        ESP_LOGW(TAG, "SD JPEG bad size: %ld", (long)fsz);
        vTaskDelete(nullptr);
        return;
    }
    rewind(f);

    uint8_t *jpeg_buf = (uint8_t *)heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_buf == nullptr) {
        jpeg_buf = (uint8_t *)heap_caps_malloc((size_t)fsz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (jpeg_buf == nullptr) {
        fclose(f);
        ESP_LOGE(TAG, "SD JPEG malloc failed");
        vTaskDelete(nullptr);
        return;
    }
    if (fread(jpeg_buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        heap_caps_free(jpeg_buf);
        fclose(f);
        ESP_LOGE(TAG, "SD JPEG read failed");
        vTaskDelete(nullptr);
        return;
    }
    fclose(f);

    soti_sd_ready_pack_t *pack = (soti_sd_ready_pack_t *)calloc(1, sizeof(soti_sd_ready_pack_t));
    if (pack == nullptr) {
        heap_caps_free(jpeg_buf);
        vTaskDelete(nullptr);
        return;
    }
    pack->app = app;
    pack->jpeg = jpeg_buf;
    pack->len = (size_t)fsz;
    if (lv_async_call(soti_sd_ready_lv_async_cb, pack) != LV_RES_OK) {
        ESP_LOGE(TAG, "lv_async_call(sd ready) failed");
        heap_caps_free(jpeg_buf);
        free(pack);
    }
    vTaskDelete(nullptr);
}

static void soti_print_task(void *arg)
{
    soti_print_job_t *job = (soti_print_job_t *)arg;
    if (job == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    SoTi *app = job->app;
    const bool with_answer = job->with_answer;
    free(job);

    const char *text = app->cachedPrintText(with_answer);
    esp_err_t err = escpos_printer_print_utf8(text);
    const char *msg = "打印 OK";
    if (err == ESP_ERR_NOT_FOUND) {
        msg = "无打印机";
    } else if (err != ESP_OK) {
        msg = "打印失败";
    }

    soti_print_status_pack_t *p = (soti_print_status_pack_t *)calloc(1, sizeof(soti_print_status_pack_t));
    if (p != nullptr) {
        p->app = app;
        strncpy(p->msg, msg, sizeof(p->msg) - 1);
        if (lv_async_call(soti_print_status_lv_async_cb, p) != LV_RES_OK) {
            free(p);
        }
    }
    power_manager_unblock_sleep("print");
    vTaskDelete(nullptr);
}

static void soti_upload_task(void *arg)
{
    soti_upload_ctx_t *ctx = (soti_upload_ctx_t *)arg;
    if (ctx == nullptr) {
        vTaskDelete(nullptr);
        return;
    }

    SoTi *app = ctx->app;
    uint8_t *jpeg = ctx->jpeg;
    size_t len = ctx->len;
    char mode_q[16];
    strncpy(mode_q, ctx->mode, sizeof(mode_q) - 1);
    mode_q[sizeof(mode_q) - 1] = '\0';
    soti_mode_t result_mode = ctx->result_mode;
    heap_caps_free(ctx);

    ESP_LOGI(TAG, "upload task start (single session) mode=%s jpeg=%u bytes", mode_q, (unsigned)len);
    parent_album_sync_pause(true);
    parent_print_sync_pause(true);
    parent_chat_bg_pause(true);

    char *abuf = (char *)calloc(1, 16384);
    if (abuf == nullptr) {
        if (jpeg != nullptr) {
            heap_caps_free(jpeg);
        }
        if (app != nullptr) {
            app->clearRequestInflight();
            soti_async_pack_t *p = (soti_async_pack_t *)calloc(1, sizeof(soti_async_pack_t));
            if (p != nullptr) {
                p->app = app;
                p->text = strdup_fallback("内存不足");
                if (p->text != nullptr) {
                    if (lv_async_call(soti_answer_lv_async_cb, p) != LV_RES_OK) {
                        ESP_LOGE(TAG, "lv_async_call(answer) failed (oom path)");
                        free(p->text);
                        free(p);
                    }
                } else {
                    free(p);
                }
            }
        }
        portENTER_CRITICAL(&s_upload_mux);
        s_upload_worker_busy = false;
        portEXIT_CRITICAL(&s_upload_mux);
        parent_album_sync_pause(false);
        parent_print_sync_pause(false);
        parent_chat_bg_pause(false);
        power_manager_unblock_sleep("soti");
        vTaskDelete(nullptr);
        return;
    }

    soti_print_sections_t print_sec = {};
    ESP_LOGI(TAG, "upload: calling worker (one photo, may use multiple HTTP connections if segmented)");
    esp_err_t e = soti_upload_jpeg_to_worker(
        jpeg, len, abuf, 16384, mode_q[0] != '\0' ? mode_q : "solve", &print_sec);
    ESP_LOGI(TAG, "upload: worker done err=%s", esp_err_to_name(e));

    if (jpeg != nullptr) {
        heap_caps_free(jpeg);
    }

    char *display = nullptr;
    if (e != ESP_OK && abuf[0] == '\0') {
        display = strdup_fallback("上传失败，请检查网络或 Worker。");
        soti_print_sections_clear(&print_sec);
    } else if (abuf[0] == '\0') {
        display = strdup_fallback("（服务器未返回文本）");
        soti_print_sections_clear(&print_sec);
    } else {
        display = soti_format_answer_for_display(abuf);
        if (display == nullptr) {
            display = strdup_fallback(abuf);
        }
        if (!print_sec.has_question && !print_sec.has_with_answer && display != nullptr) {
            soti_split_print_sections(display, result_mode, &print_sec);
        }
    }
    free(abuf);

    if (app != nullptr) {
        soti_async_pack_t *p = (soti_async_pack_t *)calloc(1, sizeof(soti_async_pack_t));
        if (p != nullptr) {
            p->app = app;
            p->text = display;
            p->mode = result_mode;
            p->print = print_sec;
            if (p->text != nullptr) {
                if (lv_async_call(soti_answer_lv_async_cb, p) != LV_RES_OK) {
                    ESP_LOGE(TAG, "lv_async_call(answer) failed");
                    free(p->text);
                    free(p);
                }
            } else {
                free(p);
            }
        } else if (display != nullptr) {
            free(display);
        }
        app->clearRequestInflight();
    } else if (display != nullptr) {
        free(display);
    }

    portENTER_CRITICAL(&s_upload_mux);
    s_upload_worker_busy = false;
    portEXIT_CRITICAL(&s_upload_mux);
    parent_album_sync_pause(false);
    parent_print_sync_pause(false);
    parent_chat_bg_pause(false);
    power_manager_unblock_sleep("soti");
    vTaskDelete(nullptr);
}

static void soti_daily_task(void *arg)
{
    soti_daily_ctx_t *ctx = (soti_daily_ctx_t *)arg;
    if (ctx == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    SoTi *app = ctx->app;
    free(ctx);

    ESP_LOGI(TAG, "daily task started");
    parent_album_sync_pause(true);
    parent_print_sync_pause(true);
    parent_chat_bg_pause(true);

    char *abuf = (char *)calloc(1, 4096);
    if (abuf == nullptr) {
        if (app != nullptr) {
            app->clearRequestInflight();
            soti_async_pack_t *p = (soti_async_pack_t *)calloc(1, sizeof(soti_async_pack_t));
            if (p != nullptr) {
                p->app = app;
                p->text = strdup_fallback("内存不足");
                if (p->text != nullptr && lv_async_call(soti_answer_lv_async_cb, p) != LV_RES_OK) {
                    free(p->text);
                    free(p);
                } else if (p->text == nullptr) {
                    free(p);
                }
            }
        }
        parent_album_sync_pause(false);
        parent_print_sync_pause(false);
        parent_chat_bg_pause(false);
        power_manager_unblock_sleep("soti");
        vTaskDelete(nullptr);
        return;
    }

    soti_print_sections_t print_sec = {};
    esp_err_t e = soti_fetch_daily_line(abuf, 4096, &print_sec);

    char *display = nullptr;
    if (e != ESP_OK && abuf[0] == '\0') {
        display = strdup_fallback("获取失败，请检查网络。");
        soti_print_sections_clear(&print_sec);
    } else if (abuf[0] == '\0') {
        display = strdup_fallback("（服务器未返回文本）");
        soti_print_sections_clear(&print_sec);
    } else {
        display = soti_format_answer_for_display(abuf);
        if (display == nullptr) {
            display = strdup_fallback(abuf);
        }
        if (!print_sec.has_question && !print_sec.has_with_answer && display != nullptr) {
            soti_split_print_sections(display, SOTI_MODE_DAILY, &print_sec);
        }
    }
    free(abuf);

    if (app != nullptr) {
        soti_async_pack_t *p = (soti_async_pack_t *)calloc(1, sizeof(soti_async_pack_t));
        if (p != nullptr) {
            p->app = app;
            p->text = display;
            p->mode = SOTI_MODE_DAILY;
            p->print = print_sec;
            if (p->text != nullptr) {
                if (lv_async_call(soti_answer_lv_async_cb, p) != LV_RES_OK) {
                    free(p->text);
                    free(p);
                }
            } else {
                free(p);
            }
        } else if (display != nullptr) {
            free(display);
        }
        app->clearRequestInflight();
    } else if (display != nullptr) {
        free(display);
    }

    parent_album_sync_pause(false);
    parent_print_sync_pause(false);
    parent_chat_bg_pause(false);
    power_manager_unblock_sleep("soti");
    vTaskDelete(nullptr);
}

SoTi::SoTi(uint16_t hor_res, uint16_t ver_res)
    : ESP_Brookesia_PhoneApp("搜题", &img_app_soti, true),
      _hor_res(hor_res),
      _ver_res(ver_res),
      _root(nullptr),
      _cam_canvas(nullptr),
      _cam_bg_buf(nullptr),
      _preview_side(nullptr),
      _mode_roller(nullptr),
      _side_mode_lbl(nullptr),
      _shutter_hint_lbl(nullptr),
      _loading_lbl(nullptr),
      _preview_paused(false),
      _request_inflight(false),
      _result_mode(SOTI_MODE_SOLVE),
      _loading_overlay(nullptr),
      _answer_layer(nullptr),
      _answer_title_lbl(nullptr),
      _answer_scroll(nullptr),
      _answer_lbl(nullptr),
      _print_bar(nullptr),
      _btn_print_question(nullptr),
      _btn_print_with_answer(nullptr),
      _print_status_lbl(nullptr)
{
    soti_print_sections_clear(&_print_cache);
}

SoTi::~SoTi() = default;

void SoTi::clearRequestInflight(void)
{
    _request_inflight = false;
}

void SoTi::armSdCardJpegUpload(const char *absolute_path)
{
    if (absolute_path == nullptr || absolute_path[0] == '\0') {
        s_armed_sd_path[0] = '\0';
        return;
    }
    strncpy(s_armed_sd_path, absolute_path, sizeof(s_armed_sd_path) - 1);
    s_armed_sd_path[sizeof(s_armed_sd_path) - 1] = '\0';
}

bool SoTi::init(void)
{
    return true;
}

void SoTi::setPreviewPaused(bool paused)
{
    if (_preview_paused == paused) {
        return;
    }
    _preview_paused = paused;
    if (paused) {
        Camera::setFrameProcessingEnabled(false);
        Camera::setPreviewCanvasOverride(nullptr);
        Camera::stopPreviewStreamingIfRunning();
        if (_cam_canvas != nullptr && lv_obj_is_valid(_cam_canvas)) {
            lv_obj_add_flag(_cam_canvas, LV_OBJ_FLAG_HIDDEN);
        }
        if (_preview_side != nullptr && lv_obj_is_valid(_preview_side)) {
            lv_obj_add_flag(_preview_side, LV_OBJ_FLAG_HIDDEN);
        }
        ESP_LOGI(TAG, "camera preview paused (loading/answer UI)");
    } else {
        if (_cam_canvas != nullptr && lv_obj_is_valid(_cam_canvas)) {
            lv_obj_clear_flag(_cam_canvas, LV_OBJ_FLAG_HIDDEN);
        }
        if (_preview_side != nullptr && lv_obj_is_valid(_preview_side)) {
            lv_obj_clear_flag(_preview_side, LV_OBJ_FLAG_HIDDEN);
        }
        restoreLivePreview();
        Camera::setFrameProcessingEnabled(true);
        if (!Camera::ensurePreviewStreaming()) {
            ESP_LOGW(TAG, "ensurePreviewStreaming failed after resume preview");
        }
        ESP_LOGI(TAG, "camera preview resumed");
    }
}

void SoTi::setLoadingHint(const char *line1)
{
    if (_loading_lbl == nullptr || !lv_obj_is_valid(_loading_lbl) || line1 == nullptr) {
        return;
    }
    if (_result_mode == SOTI_MODE_DAILY) {
        lv_label_set_text_fmt(_loading_lbl, "%s\n请稍候", line1);
    } else {
        lv_label_set_text_fmt(_loading_lbl, "%s\n请稍候约 30 秒", line1);
    }
}

void SoTi::refreshSideModeHint(void)
{
    const soti_mode_t mode = selectedModeFromRoller();
    if (_side_mode_lbl != nullptr && lv_obj_is_valid(_side_mode_lbl)) {
        lv_label_set_text_fmt(_side_mode_lbl, "当前：%s", soti_mode_display_name(mode));
    }
    if (_shutter_hint_lbl != nullptr && lv_obj_is_valid(_shutter_hint_lbl)) {
        if (mode == SOTI_MODE_DAILY) {
            lv_label_set_text(_shutter_hint_lbl, "按快门获取");
        } else {
            lv_label_set_text(_shutter_hint_lbl, "按快门拍照");
        }
    }
}

void SoTi::showLoadingUi(bool show)
{
    if (_loading_overlay == nullptr || !lv_obj_is_valid(_loading_overlay)) {
        return;
    }
    if (show) {
        setPreviewPaused(true);
        lv_obj_clear_flag(_loading_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(_loading_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(_loading_overlay);
    } else {
        lv_obj_add_flag(_loading_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_loading_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_background(_loading_overlay);
        if (_preview_side != nullptr && lv_obj_is_valid(_preview_side)) {
            lv_obj_move_foreground(_preview_side);
        }
        const bool answer_visible =
            _answer_layer != nullptr && lv_obj_is_valid(_answer_layer) &&
            !lv_obj_has_flag(_answer_layer, LV_OBJ_FLAG_HIDDEN);
        if (!answer_visible) {
            setPreviewPaused(false);
        }
    }
}

soti_mode_t SoTi::selectedModeFromRoller() const
{
    if (_mode_roller == nullptr || !lv_obj_is_valid(_mode_roller)) {
        return SOTI_MODE_SOLVE;
    }
    uint16_t sel = lv_roller_get_selected(_mode_roller);
    if (sel >= (uint16_t)SOTI_MODE_COUNT) {
        return SOTI_MODE_SOLVE;
    }
    return (soti_mode_t)sel;
}

void SoTi::showAnswerUi(const char *utf8)
{
    if (_answer_layer == nullptr || !lv_obj_is_valid(_answer_layer) || _answer_lbl == nullptr ||
        !lv_obj_is_valid(_answer_lbl)) {
        return;
    }
    setPreviewPaused(true);
    if (_answer_title_lbl != nullptr && lv_obj_is_valid(_answer_title_lbl)) {
        lv_label_set_text(_answer_title_lbl, soti_mode_display_name(_result_mode));
    }
    lv_label_set_text(_answer_lbl, (utf8 != nullptr && utf8[0] != '\0') ? utf8 : "（无内容）");
    lv_obj_clear_flag(_answer_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_answer_layer);
    lv_obj_scroll_to_y(_answer_scroll, 0, LV_ANIM_OFF);
    refreshPrintButtons();
}

void SoTi::clearPrintCache(void)
{
    soti_print_sections_clear(&_print_cache);
}

void SoTi::setPrintCache(soti_mode_t mode, const soti_print_sections_t *sections)
{
    _result_mode = mode;
    clearPrintCache();
    if (sections != nullptr) {
        _print_cache = *sections;
    }
    refreshPrintButtons();
}

const char *SoTi::cachedPrintText(bool with_answer) const
{
    return with_answer ? _print_cache.with_answer : _print_cache.question;
}

void SoTi::postPrintStatusAsync(const char *msg)
{
    if (_print_status_lbl != nullptr && lv_obj_is_valid(_print_status_lbl) && msg != nullptr) {
        lv_label_set_text(_print_status_lbl, msg);
    }
}

void SoTi::refreshPrintButtons(void)
{
    const bool printer = uart_escpos_printer_ready() || usb_escpos_printer_ready();
    if (_btn_print_question != nullptr && lv_obj_is_valid(_btn_print_question)) {
        if (printer && _print_cache.has_question) {
            lv_obj_clear_state(_btn_print_question, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(_btn_print_question, LV_STATE_DISABLED);
        }
    }
    if (_btn_print_with_answer != nullptr && lv_obj_is_valid(_btn_print_with_answer)) {
        if (_result_mode == SOTI_MODE_DAILY) {
            lv_obj_add_flag(_btn_print_with_answer, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(_btn_print_with_answer, LV_OBJ_FLAG_HIDDEN);
            if (printer && _print_cache.has_with_answer) {
                lv_obj_clear_state(_btn_print_with_answer, LV_STATE_DISABLED);
            } else {
                lv_obj_add_state(_btn_print_with_answer, LV_STATE_DISABLED);
            }
        }
    }
    if (_print_status_lbl != nullptr && lv_obj_is_valid(_print_status_lbl)) {
        lv_label_set_text(_print_status_lbl, "");
    }
}

void SoTi::buildAuxiliaryUi(lv_coord_t vw, lv_coord_t vh)
{
    _loading_overlay = lv_obj_create(_root);
    lv_obj_set_size(_loading_overlay, vw, vh);
    lv_obj_align(_loading_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(_loading_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(_loading_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(_loading_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(_loading_overlay, LV_OBJ_FLAG_CLICKABLE);
    _loading_lbl = lv_label_create(_loading_overlay);
    lv_label_set_text(_loading_lbl, "识别中\n请稍候约 30 秒");
    lv_obj_set_style_text_color(_loading_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(_loading_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_align(_loading_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(_loading_lbl, 8, 0);
    lv_obj_align(_loading_lbl, LV_ALIGN_CENTER, 0, -36);

    lv_obj_t *cancel_btn = lv_btn_create(_loading_overlay);
    lv_obj_set_size(cancel_btn, 120, 44);
    lv_obj_align(cancel_btn, LV_ALIGN_CENTER, 0, 48);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(cancel_btn, on_loading_cancel, LV_EVENT_CLICKED, this);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "取消");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_center(cancel_lbl);

    _answer_layer = lv_obj_create(_root);
    lv_obj_set_size(_answer_layer, vw, vh);
    lv_obj_set_style_bg_color(_answer_layer, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_bg_opa(_answer_layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(_answer_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(_answer_layer, 8, 0);
    lv_obj_clear_flag(_answer_layer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(_answer_layer);
    lv_obj_set_size(hdr, vw - 16, 48);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0xDDDDDD), LV_PART_MAIN);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back = lv_btn_create(hdr);
    lv_obj_set_size(back, 72, 36);
    lv_obj_add_event_cb(back, on_answer_back, LV_EVENT_CLICKED, this);
    lv_obj_t *lb = lv_label_create(back);
    lv_label_set_text(lb, "返回");
    lv_obj_set_style_text_font(lb, &lv_font_ui_zh_22, 0);
    lv_obj_center(lb);

    _answer_title_lbl = lv_label_create(hdr);
    lv_label_set_text(_answer_title_lbl, "搜题");
    lv_obj_set_style_text_font(_answer_title_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_answer_title_lbl, lv_color_hex(0x111111), 0);
    lv_obj_set_style_pad_left(_answer_title_lbl, 12, 0);
    lv_label_set_long_mode(_answer_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(_answer_title_lbl, 1);

    _answer_scroll = lv_obj_create(_answer_layer);
    lv_obj_set_size(_answer_scroll, vw - 16, (lv_coord_t)(vh - 128));
    lv_obj_align(_answer_scroll, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_flag(_answer_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(_answer_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(_answer_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_answer_scroll, 0, 0);
    lv_obj_set_style_pad_all(_answer_scroll, 4, 0);

    lv_obj_t *card = lv_obj_create(_answer_scroll);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    _answer_lbl = lv_label_create(card);
    lv_label_set_long_mode(_answer_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_answer_lbl, LV_PCT(100));
    lv_obj_set_style_text_font(_answer_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_answer_lbl, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_line_space(_answer_lbl, 12, 0);
    lv_obj_set_style_pad_bottom(_answer_lbl, 8, 0);
    lv_obj_set_style_text_letter_space(_answer_lbl, 0, 0);

    _print_bar = lv_obj_create(_answer_layer);
    lv_obj_set_size(_print_bar, vw - 16, 56);
    lv_obj_align(_print_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(_print_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_print_bar, 0, 0);
    lv_obj_set_style_pad_all(_print_bar, 0, 0);
    lv_obj_set_flex_flow(_print_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_print_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(_print_bar, LV_OBJ_FLAG_SCROLLABLE);

    _btn_print_question = lv_btn_create(_print_bar);
    lv_obj_set_height(_btn_print_question, 40);
    lv_obj_set_flex_grow(_btn_print_question, 1);
    lv_obj_set_style_bg_color(_btn_print_question, lv_color_hex(0x2563EB), 0);
    lv_obj_t *lbl_q = lv_label_create(_btn_print_question);
    lv_label_set_text(lbl_q, "仅题目");
    lv_obj_set_style_text_font(lbl_q, &lv_font_ui_zh_22, 0);
    lv_obj_center(lbl_q);
    lv_obj_add_event_cb(_btn_print_question, on_print_question_clicked, LV_EVENT_CLICKED, this);

    _btn_print_with_answer = lv_btn_create(_print_bar);
    lv_obj_set_height(_btn_print_with_answer, 40);
    lv_obj_set_flex_grow(_btn_print_with_answer, 1);
    lv_obj_set_style_bg_color(_btn_print_with_answer, lv_color_hex(0x2D6A4F), 0);
    lv_obj_t *lbl_a = lv_label_create(_btn_print_with_answer);
    lv_label_set_text(lbl_a, "含答案");
    lv_obj_set_style_text_font(lbl_a, &lv_font_ui_zh_22, 0);
    lv_obj_center(lbl_a);
    lv_obj_add_event_cb(_btn_print_with_answer, on_print_with_answer_clicked, LV_EVENT_CLICKED, this);

    _print_status_lbl = lv_label_create(_answer_layer);
    lv_label_set_text(_print_status_lbl, "");
    lv_obj_set_style_text_font(_print_status_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_print_status_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align(_print_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -62);
}

void SoTi::restoreLivePreview()
{
    Camera::setPreviewCanvasOverride(_cam_canvas);
}

void SoTi::deliverSdJpegForUpload(uint8_t *jpeg, size_t len)
{
    startUploadTask(jpeg, len, selectedModeFromRoller());
}

void SoTi::tryConsumeSdCardUpload()
{
    if (s_armed_sd_path[0] == '\0') {
        return;
    }
    if (xTaskCreate(soti_sd_consume_task, "soti_sd", 8192, this, 5, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "soti_sd task create failed");
        s_armed_sd_path[0] = '\0';
    }
}

void SoTi::startUploadTask(uint8_t *jpeg, size_t jpeg_len, soti_mode_t mode)
{
    if (_request_inflight) {
        ESP_LOGW(TAG, "upload: request already in flight, drop duplicate");
        heap_caps_free(jpeg);
        return;
    }
    portENTER_CRITICAL(&s_upload_mux);
    if (s_upload_worker_busy) {
        portEXIT_CRITICAL(&s_upload_mux);
        ESP_LOGW(TAG, "upload: worker busy, drop duplicate");
        heap_caps_free(jpeg);
        return;
    }
    s_upload_worker_busy = true;
    portEXIT_CRITICAL(&s_upload_mux);
    _request_inflight = true;
    _result_mode = mode;
    setLoadingHint(soti_mode_loading_hint(mode));
    showLoadingUi(true);

    soti_upload_ctx_t *ctx =
        (soti_upload_ctx_t *)heap_caps_malloc(sizeof(soti_upload_ctx_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ctx == nullptr) {
        heap_caps_free(jpeg);
        _request_inflight = false;
        portENTER_CRITICAL(&s_upload_mux);
        s_upload_worker_busy = false;
        portEXIT_CRITICAL(&s_upload_mux);
        showLoadingUi(false);
        setPreviewPaused(false);
        ESP_LOGE(TAG, "upload ctx alloc failed");
        return;
    }
    ctx->jpeg = jpeg;
    ctx->len = jpeg_len;
    ctx->app = this;
    ctx->result_mode = mode;
    const char *mq = soti_mode_query_value(mode);
    strncpy(ctx->mode, mq, sizeof(ctx->mode) - 1);
    ctx->mode[sizeof(ctx->mode) - 1] = '\0';

    if (xTaskCreate(soti_upload_task, "soti_r2_up", 18 * 1024, ctx, 5, nullptr) != pdPASS) {
        heap_caps_free(jpeg);
        heap_caps_free(ctx);
        _request_inflight = false;
        portENTER_CRITICAL(&s_upload_mux);
        s_upload_worker_busy = false;
        portEXIT_CRITICAL(&s_upload_mux);
        showLoadingUi(false);
        setPreviewPaused(false);
        ESP_LOGE(TAG, "upload task create failed");
    } else {
        power_manager_block_sleep("soti");
    }
}

void SoTi::startDailyTask(void)
{
    if (_request_inflight) {
        ESP_LOGW(TAG, "daily: reset stale in-flight and retry");
        _request_inflight = false;
        showLoadingUi(false);
        setPreviewPaused(false);
    }
    _request_inflight = true;
    _result_mode = SOTI_MODE_DAILY;
    setLoadingHint(soti_mode_loading_hint(SOTI_MODE_DAILY));
    showLoadingUi(true);

    soti_daily_ctx_t *ctx = (soti_daily_ctx_t *)malloc(sizeof(soti_daily_ctx_t));
    if (ctx == nullptr) {
        _request_inflight = false;
        showLoadingUi(false);
        setPreviewPaused(false);
        return;
    }
    ctx->app = this;
    /* esp_http_client + lwIP need headroom; avoid stack canary fault (was 16KB). */
    if (xTaskCreate(soti_daily_task, "soti_daily", 24 * 1024, ctx, 5, nullptr) != pdPASS) {
        free(ctx);
        _request_inflight = false;
        showLoadingUi(false);
        setPreviewPaused(false);
        ESP_LOGE(TAG, "daily task create failed");
    } else {
        power_manager_block_sleep("soti");
    }
}

bool SoTi::run(void)
{
    _request_inflight = false;
    _preview_paused = false;
    portENTER_CRITICAL(&s_upload_mux);
    s_upload_worker_busy = false;
    portEXIT_CRITICAL(&s_upload_mux);

    lv_area_t area = getVisualArea();
    const lv_coord_t vw = area.x2 - area.x1;
    const lv_coord_t vh = area.y2 - area.y1;

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_size(scr, vw, vh);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    _root = lv_obj_create(scr);
    lv_obj_set_size(_root, vw, vh);
    lv_obj_set_pos(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_root, 4, 0);
    lv_obj_set_style_border_color(_root, lv_color_black(), 0);
    lv_obj_set_style_border_opa(_root, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_set_style_radius(_root, 0, 0);

    if (!Camera::ensurePreviewStreaming()) {
        ESP_LOGW(TAG, "Camera stream not started (preview may delay)");
    }

    _cam_canvas = lv_canvas_create(_root);
    lv_obj_set_size(_cam_canvas, _hor_res, _ver_res);
    lv_obj_align(_cam_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_move_background(_cam_canvas);

    const size_t cam_px = (size_t)_hor_res * (size_t)_ver_res * sizeof(lv_color_t);
    _cam_bg_buf = heap_caps_malloc(cam_px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (_cam_bg_buf == nullptr) {
        _cam_bg_buf = heap_caps_malloc(cam_px, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (_cam_bg_buf == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate camera canvas placeholder");
        lv_obj_del(_root);
        _root = nullptr;
        _cam_canvas = nullptr;
        return false;
    }
    lv_canvas_set_buffer(_cam_canvas, _cam_bg_buf, _hor_res, _ver_res, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(_cam_canvas, lv_color_black(), LV_OPA_COVER);

    _preview_side = lv_obj_create(_root);
    lv_obj_t *side = _preview_side;
    lv_obj_set_width(side, 152);
    lv_obj_set_height(side, LV_PCT(100));
    lv_obj_align(side, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_clear_flag(side, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(side, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(side, 0, 0);
    lv_obj_set_style_pad_all(side, 4, 0);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(side, 12, 0);
    lv_obj_add_flag(side, LV_OBJ_FLAG_CLICKABLE);

    _side_mode_lbl = lv_label_create(side);
    lv_label_set_text(_side_mode_lbl, "当前：搜题");
    lv_obj_set_width(_side_mode_lbl, 144);
    lv_label_set_long_mode(_side_mode_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(_side_mode_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_side_mode_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_align(_side_mode_lbl, LV_TEXT_ALIGN_CENTER, 0);

    _mode_roller = lv_roller_create(side);
    lv_roller_set_options(_mode_roller, soti_roller_options(), LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(_mode_roller, 144);
    lv_obj_set_style_text_font(_mode_roller, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_line_space(_mode_roller, 6, 0);
    lv_roller_set_visible_row_count(_mode_roller, 3);
    lv_obj_add_event_cb(_mode_roller, on_mode_roller_changed, LV_EVENT_VALUE_CHANGED, this);

    lv_obj_t *daily_btn = lv_btn_create(side);
    lv_obj_set_size(daily_btn, 144, 40);
    lv_obj_add_event_cb(daily_btn, on_daily_click, LV_EVENT_CLICKED, this);
    lv_obj_t *daily_lbl = lv_label_create(daily_btn);
    lv_label_set_text(daily_lbl, "每日一句");
    lv_obj_set_style_text_font(daily_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_add_flag(daily_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(daily_lbl);

    lv_obj_t *sh_outer = lv_obj_create(side);
    lv_obj_set_size(sh_outer, 88, 88);
    lv_obj_set_style_radius(sh_outer, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sh_outer, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sh_outer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sh_outer, 0, 0);
    lv_obj_set_style_pad_all(sh_outer, 0, 0);
    lv_obj_clear_flag(sh_outer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sh_outer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sh_outer, on_shutter_click, LV_EVENT_CLICKED, this);

    lv_obj_t *sh_inner = lv_obj_create(sh_outer);
    lv_obj_set_size(sh_inner, 68, 68);
    lv_obj_center(sh_inner);
    lv_obj_set_style_radius(sh_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_grad_dir(sh_inner, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_color(sh_inner, lv_color_hex(0x6AB4FF), 0);
    lv_obj_set_style_bg_grad_color(sh_inner, lv_color_hex(0x2563CC), 0);
    lv_obj_set_style_border_width(sh_inner, 0, 0);
    lv_obj_clear_flag(sh_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sh_inner, LV_OBJ_FLAG_EVENT_BUBBLE);

    _shutter_hint_lbl = lv_label_create(side);
    lv_label_set_text(_shutter_hint_lbl, "按快门拍照");
    lv_obj_set_style_text_font(_shutter_hint_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_shutter_hint_lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_align(_shutter_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);

    buildAuxiliaryUi(vw, vh);
    refreshSideModeHint();

    lv_obj_move_foreground(side);
    if (_loading_overlay != nullptr) {
        lv_obj_move_background(_loading_overlay);
        lv_obj_clear_flag(_loading_overlay, LV_OBJ_FLAG_CLICKABLE);
    }
    if (_answer_layer != nullptr) {
        lv_obj_move_background(_answer_layer);
    }

    if (!Camera::ensureJpegEncoderForHw()) {
        ESP_LOGW(TAG, "JPEG encoder unavailable; shutter may fail");
    }

    Camera::setPreviewCanvasOverride(_cam_canvas);
    Camera::setFrameProcessingEnabled(true);

    if (bsp_display_lock(100)) {
        lv_obj_invalidate(_cam_canvas);
        lv_refr_now(NULL);
        bsp_display_unlock();
    }

    if (_answer_layer != nullptr && lv_obj_is_valid(_answer_layer)) {
        lv_obj_add_flag(_answer_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (_loading_overlay != nullptr && lv_obj_is_valid(_loading_overlay)) {
        lv_obj_add_flag(_loading_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(_loading_overlay, LV_OBJ_FLAG_CLICKABLE);
    }

    tryConsumeSdCardUpload();

    return true;
}

bool SoTi::pause(void)
{
    restoreLivePreview();
    Camera::setPreviewCanvasOverride(nullptr);
    Camera::setFrameProcessingEnabled(false);
    /* Same HW JPEG block as video decoder; release so MJPEG player can open jpeg_new_decoder_engine(). */
    Camera::releaseJpegEncoderHw();
    showLoadingUi(false);
    if (_preview_paused) {
        setPreviewPaused(false);
    }
    return true;
}

bool SoTi::resume(void)
{
    const bool answer_visible =
        _answer_layer != nullptr && lv_obj_is_valid(_answer_layer) &&
        !lv_obj_has_flag(_answer_layer, LV_OBJ_FLAG_HIDDEN);
    const bool loading_visible =
        _loading_overlay != nullptr && lv_obj_is_valid(_loading_overlay) &&
        !lv_obj_has_flag(_loading_overlay, LV_OBJ_FLAG_HIDDEN);
    if (answer_visible || loading_visible) {
        setPreviewPaused(true);
    } else {
        if (_cam_canvas != nullptr && lv_obj_is_valid(_cam_canvas)) {
            Camera::setPreviewCanvasOverride(_cam_canvas);
        }
        Camera::setFrameProcessingEnabled(true);
        (void)Camera::ensurePreviewStreaming();
    }
    (void)Camera::ensureJpegEncoderForHw();
    tryConsumeSdCardUpload();
    return true;
}

bool SoTi::back(void)
{
    notifyCoreClosed();
    return true;
}

bool SoTi::close(void)
{
    restoreLivePreview();
    Camera::setPreviewCanvasOverride(nullptr);
    Camera::setFrameProcessingEnabled(false);
    Camera::releaseJpegEncoderHw();
    if (_cam_bg_buf != nullptr) {
        heap_caps_free(_cam_bg_buf);
        _cam_bg_buf = nullptr;
    }
    _cam_canvas = nullptr;
    _preview_side = nullptr;
    _mode_roller = nullptr;
    _side_mode_lbl = nullptr;
    _shutter_hint_lbl = nullptr;
    _loading_lbl = nullptr;
    _preview_paused = false;
    _request_inflight = false;
    _root = nullptr;
    _loading_overlay = nullptr;
    _answer_layer = nullptr;
    _answer_title_lbl = nullptr;
    _answer_scroll = nullptr;
    _answer_lbl = nullptr;
    _print_bar = nullptr;
    _btn_print_question = nullptr;
    _btn_print_with_answer = nullptr;
    _print_status_lbl = nullptr;
    clearPrintCache();
    return true;
}

void SoTi::on_upload_confirm_msgbox(lv_event_t *e)
{
    soti_confirm_upload_t *c = (soti_confirm_upload_t *)lv_event_get_user_data(e);
    if (c == nullptr || lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *mbox = lv_event_get_current_target(e);
    const int btn = lv_msgbox_get_active_btn(mbox);
    ESP_LOGI(TAG, "Upload confirm msgbox btn=%d", btn);
    lv_msgbox_close(mbox);
    lv_obj_remove_event_cb(mbox, on_upload_confirm_msgbox);

    if (btn != 1) {
        heap_caps_free(c->jpeg);
        free(c);
        return;
    }

    if (c->app->_request_inflight) {
        ESP_LOGW(TAG, "upload confirm ignored: already in flight");
        heap_caps_free(c->jpeg);
        free(c);
        return;
    }

    c->app->startUploadTask(c->jpeg, c->jpeg_len, c->app->selectedModeFromRoller());
    free(c);
}

void SoTi::on_loading_cancel(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    ESP_LOGI(TAG, "loading cancelled by user");
    app->clearRequestInflight();
    portENTER_CRITICAL(&s_upload_mux);
    s_upload_worker_busy = false;
    portEXIT_CRITICAL(&s_upload_mux);
    app->showLoadingUi(false);
}

void SoTi::on_mode_roller_changed(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    app->refreshSideModeHint();
}

void SoTi::on_daily_click(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    if (app->_mode_roller != nullptr && lv_obj_is_valid(app->_mode_roller)) {
        lv_roller_set_selected(app->_mode_roller, (uint16_t)SOTI_MODE_DAILY, LV_ANIM_ON);
        app->refreshSideModeHint();
    }
    ESP_LOGI(TAG, "daily button clicked");
    app->startDailyTask();
}

void SoTi::on_shutter_click(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    if (app->selectedModeFromRoller() == SOTI_MODE_DAILY) {
        ESP_LOGI(TAG, "shutter -> daily (roller)");
        app->startDailyTask();
        return;
    }
    if (!Camera::ensureJpegEncoderForHw()) {
        ESP_LOGW(TAG, "Shutter: JPEG encoder not ready");
        return;
    }
    if (!Camera::requestSoTiSnapshotAndWait(4000)) {
        ESP_LOGW(TAG, "Shutter: timeout or failed");
        return;
    }

    size_t jlen = 0;
    uint8_t *jpeg = Camera::takeSoTiUploadJpeg(&jlen);

    if (jpeg == nullptr || jlen == 0) {
        ESP_LOGW(TAG, "No JPEG after snapshot");
        return;
    }

    ESP_LOGI(TAG, "Snapshot JPEG ready (%u bytes), opening confirm", (unsigned)jlen);

    if (jlen > (size_t)SOTI_MAX_UPLOAD_JPEG_BYTES) {
        ESP_LOGW(TAG, "JPEG too large for SDIO upload (%u > %d)", (unsigned)jlen, SOTI_MAX_UPLOAD_JPEG_BYTES);
        heap_caps_free(jpeg);
        static const char *btns[] = {"确定", ""};
        lv_obj_t *mbox = lv_msgbox_create(NULL, "图片过大", "照片过大无法上传，请靠近题目或改善光线后重拍。", btns, false);
        if (mbox != nullptr) {
            soti_apply_zh_font_22(mbox);
            lv_obj_center(mbox);
        }
        return;
    }

    soti_confirm_upload_t *c = (soti_confirm_upload_t *)malloc(sizeof(soti_confirm_upload_t));
    if (c == nullptr) {
        heap_caps_free(jpeg);
        ESP_LOGE(TAG, "confirm ctx malloc failed");
        return;
    }
    c->app = app;
    c->jpeg = jpeg;
    c->jpeg_len = jlen;

    /* NULL parent: LVGL puts backdrop on lv_layer_top() so the box is above the app (lv_layer_sys() is often covered). */
    const soti_mode_t mode = app->selectedModeFromRoller();
    const char *prompt =
        (mode == SOTI_MODE_DAILY) ? "获取今日一句？" : "上传当前照片？";
    static const char *btns[] = {"取消", "确定", ""};
    lv_obj_t *mbox = lv_msgbox_create(NULL, soti_mode_display_name(mode), prompt, btns, false);
    if (mbox == nullptr) {
        heap_caps_free(jpeg);
        free(c);
        ESP_LOGE(TAG, "lv_msgbox_create failed");
        return;
    }
    soti_apply_zh_font_22(mbox);
    lv_obj_add_event_cb(mbox, on_upload_confirm_msgbox, LV_EVENT_VALUE_CHANGED, c);
    lv_obj_center(mbox);
}

void SoTi::on_answer_back(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr || app->_answer_layer == nullptr || !lv_obj_is_valid(app->_answer_layer)) {
        return;
    }
    lv_obj_add_flag(app->_answer_layer, LV_OBJ_FLAG_HIDDEN);
    app->clearRequestInflight();
    portENTER_CRITICAL(&s_upload_mux);
    s_upload_worker_busy = false;
    portEXIT_CRITICAL(&s_upload_mux);
    app->setPreviewPaused(false);
}

static void soti_start_print(SoTi *app, bool with_answer)
{
    if (app == nullptr) {
        return;
    }
    if (!uart_escpos_printer_ready() && !usb_escpos_printer_ready()) {
        app->postPrintStatusAsync("无打印机");
        return;
    }
    const char *text = app->cachedPrintText(with_answer);
    if (text == nullptr || text[0] == '\0') {
        app->postPrintStatusAsync("无内容");
        return;
    }

    soti_print_job_t *job = (soti_print_job_t *)calloc(1, sizeof(soti_print_job_t));
    if (job == nullptr) {
        app->postPrintStatusAsync("内存不足");
        return;
    }
    job->app = app;
    job->with_answer = with_answer;
    app->postPrintStatusAsync("打印中");
    if (xTaskCreate(soti_print_task, "soti_print", 8192, job, 5, nullptr) != pdPASS) {
        free(job);
        app->postPrintStatusAsync("无法启动打印");
        return;
    }
    power_manager_block_sleep("print");
}

void SoTi::on_print_question_clicked(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    soti_start_print(app, false);
}

void SoTi::on_print_with_answer_clicked(lv_event_t *e)
{
    SoTi *app = (SoTi *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    soti_start_print(app, true);
}
