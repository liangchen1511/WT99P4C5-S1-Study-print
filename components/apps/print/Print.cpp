/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Print.hpp"
#include "lv_font_ui_zh.h"
#include "parent_guard.hpp"
#include "parent_print_sync.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static const char *TAG = "PrintApp";

LV_IMG_DECLARE(img_app_print);

struct print_status_ctx {
    Print *app;
    char line[96];
};

struct print_refresh_job {
    Print *app;
};

struct print_run_job {
    Print *app;
    int job_id;
};

static void status_async_cb(void *ud)
{
    print_status_ctx *c = (print_status_ctx *)ud;
    if (c != nullptr && c->app != nullptr) {
        c->app->set_status(c->line);
    }
    free(c);
}

static void refresh_async_cb(void *ud)
{
    print_refresh_job *job = (print_refresh_job *)ud;
    if (job != nullptr && job->app != nullptr) {
        job->app->rebuild_job_list();
    }
    free(job);
}

static void print_done_async_cb(void *ud)
{
    print_run_job *job = (print_run_job *)ud;
    if (job != nullptr && job->app != nullptr) {
        job->app->set_printing(false);
        job->app->request_refresh();
    }
    free(job);
}

static void refresh_task(void *arg)
{
    print_refresh_job *job = (print_refresh_job *)arg;
    if (job == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    (void)parent_print_sync_refresh_pending();
    if (lv_async_call(refresh_async_cb, job) != LV_RES_OK) {
        free(job);
    }
    vTaskDelete(nullptr);
}

static void print_task(void *arg)
{
    print_run_job *job = (print_run_job *)arg;
    if (job == nullptr) {
        vTaskDelete(nullptr);
        return;
    }
    if (job->app != nullptr) {
        job->app->set_printing(true);
        job->app->set_status("打印中…");
    }
    esp_err_t err = parent_print_sync_print_job(job->job_id);
    if (job->app != nullptr && err != ESP_OK) {
        job->app->set_status("打印失败");
    }
    if (lv_async_call(print_done_async_cb, job) != LV_RES_OK) {
        free(job);
    }
    vTaskDelete(nullptr);
}

void Print::print_status_cb(const char *line, void *user)
{
    Print *app = (Print *)user;
    if (app == nullptr || line == nullptr) {
        return;
    }
    print_status_ctx *c = (print_status_ctx *)calloc(1, sizeof(*c));
    if (c == nullptr) {
        return;
    }
    c->app = app;
    snprintf(c->line, sizeof(c->line), "%s", line);
    if (lv_async_call(status_async_cb, c) != LV_RES_OK) {
        free(c);
    }
}

Print::Print()
    : ESP_Brookesia_PhoneApp("打印", &img_app_print, true)
{
}

Print::~Print() = default;

bool Print::init(void)
{
    static bool hooks_set;
    if (!hooks_set) {
        parent_print_sync_set_status_callback(print_status_cb, this);
        hooks_set = true;
    }
    return true;
}

void Print::set_status(const char *msg)
{
    if (_lbl_status == nullptr) {
        return;
    }
    if (msg == nullptr || msg[0] == '\0') {
        lv_label_set_text(_lbl_status, "");
        lv_obj_add_flag(_lbl_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(_lbl_status, msg);
        lv_obj_clear_flag(_lbl_status, LV_OBJ_FLAG_HIDDEN);
    }
}

void Print::set_printing(bool on)
{
    _printing = on;
    rebuild_job_list();
}

void Print::rebuild_job_list(void)
{
    if (_list == nullptr) {
        return;
    }
    lv_obj_clean(_list);
    const size_t n = parent_print_sync_job_count();
    if (n == 0) {
        lv_obj_t *empty = lv_label_create(_list);
        lv_label_set_text(empty, "暂无待打印任务\n家长站「远程打印」上传后点刷新");
        lv_obj_set_style_text_font(empty, &lv_font_ui_zh_22, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0xaaaaaa), 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, lv_pct(100));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        parent_print_job_t job = {};
        if (!parent_print_sync_get_job(i, &job)) {
            continue;
        }
        lv_obj_t *row = lv_obj_create(_list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_20, 0);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        char txt[96];
        const char *label = (job.type == PARENT_PRINT_JOB_TYPE_TEXT && job.text_title[0] != '\0')
                                ? job.text_title
                                : job.name;
        snprintf(txt, sizeof(txt), "#%d %s\n%s", job.id,
                 job.type == PARENT_PRINT_JOB_TYPE_TEXT ? "文字" : "图片", label);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_ui_zh_22, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_flex_grow(lbl, 1);

        lv_obj_set_user_data(row, this);
        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 72, 36);
        lv_obj_t *btl = lv_label_create(btn);
        lv_label_set_text(btl, (_printing || parent_print_sync_is_busy()) ? "…" : "打印");
        lv_obj_set_style_text_font(btl, &lv_font_ui_zh_22, 0);
        lv_obj_set_style_text_color(btl, lv_color_white(), 0);
        lv_obj_center(btl);
        if (_printing || parent_print_sync_is_busy()) {
            lv_obj_add_state(btn, LV_STATE_DISABLED);
        }
        lv_obj_add_event_cb(btn, on_print_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)job.id);
    }
}

void Print::request_refresh(void)
{
    print_refresh_job *job = (print_refresh_job *)calloc(1, sizeof(*job));
    if (job == nullptr) {
        rebuild_job_list();
        return;
    }
    job->app = this;
    if (xTaskCreate(refresh_task, "prt_refresh", 8192, job, 4, nullptr) != pdPASS) {
        free(job);
        rebuild_job_list();
    }
}

void Print::on_refresh_clicked(lv_event_t *e)
{
    Print *app = (Print *)lv_event_get_user_data(e);
    if (app == nullptr) {
        return;
    }
    app->set_status("刷新中…");
    app->request_refresh();
}

void Print::on_print_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *row = lv_obj_get_parent(btn);
    Print *app = row != nullptr ? (Print *)lv_obj_get_user_data(row) : nullptr;
    int job_id = (int)(intptr_t)lv_event_get_user_data(e);
    if (app == nullptr || job_id <= 0) {
        return;
    }
    if (app->_printing || parent_print_sync_is_busy()) {
        app->set_status("打印中，请稍候");
        return;
    }
    print_run_job *job = (print_run_job *)calloc(1, sizeof(*job));
    if (job == nullptr) {
        app->set_status("内存不足");
        return;
    }
    job->app = app;
    job->job_id = job_id;
    if (xTaskCreate(print_task, "prt_job", 24576, job, 5, nullptr) != pdPASS) {
        free(job);
        app->set_status("无法启动打印");
    }
}

bool Print::run(void)
{
    if (!parent_guard_app_run("print")) {
        return false;
    }
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
    lv_obj_center(_root);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 12, 0);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *top = lv_obj_create(_root);
    lv_obj_set_width(top, lv_pct(100));
    lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(top, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, "家长远程打印");
    lv_obj_set_style_text_font(title, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t *btn_ref = lv_btn_create(top);
    lv_obj_set_size(btn_ref, 72, 36);
    lv_obj_t *lbl_ref = lv_label_create(btn_ref);
    lv_label_set_text(lbl_ref, "刷新");
    lv_obj_set_style_text_font(lbl_ref, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(lbl_ref, lv_color_white(), 0);
    lv_obj_center(lbl_ref);
    lv_obj_add_event_cb(btn_ref, on_refresh_clicked, LV_EVENT_CLICKED, this);

    _lbl_status = lv_label_create(_root);
    lv_label_set_text(_lbl_status, "");
    lv_obj_add_flag(_lbl_status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(_lbl_status, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_lbl_status, lv_color_hex(0x88ccff), 0);
    lv_obj_set_width(_lbl_status, lv_pct(100));

    _list = lv_obj_create(_root);
    lv_obj_set_width(_list, lv_pct(100));
    lv_obj_set_flex_grow(_list, 1);
    lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_list, 0, 0);
    lv_obj_set_style_pad_all(_list, 0, 0);
    lv_obj_set_style_pad_row(_list, 8, 0);
    lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(_list, LV_OBJ_FLAG_SCROLLABLE);

    _lbl_hint = lv_label_create(_root);
    lv_label_set_text(_lbl_hint,
                      "SoTi 答案页亦可打印。\n"
                      "TTL：GPIO4->RX GPIO5<-TX；打印机CTS->GPIO6 流控；115200。");
    lv_obj_set_style_text_font(_lbl_hint, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(_lbl_hint, lv_color_hex(0x666666), 0);
    lv_label_set_long_mode(_lbl_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_lbl_hint, lv_pct(100));

    request_refresh();
    return true;
}

bool Print::pause(void)
{
    return true;
}

bool Print::resume(void)
{
    request_refresh();
    return true;
}

bool Print::back(void)
{
    notifyCoreClosed();
    return true;
}

bool Print::close(void)
{
    _root = nullptr;
    _lbl_status = nullptr;
    _list = nullptr;
    _lbl_hint = nullptr;
    _printing = false;
    return true;
}
