/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ParentChat.hpp"
#include "parent_chat_api.hpp"
#include "parent_chat_launcher.hpp"
#include "parent_chat_pinyin_dict.h"
#include "lv_font_ui_zh.h"

#include "lvgl.h"

#include <cstring>
#include <ctime>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LV_IMG_DECLARE(img_app_parent_chat);

static const char *TAG = "ParentChat";
static const lv_coord_t PCHAT_CAND_H = 48;
static const lv_coord_t PCHAT_INPUT_ROW_H = 56;

#if LV_USE_IME_PINYIN
static void styleCandPanel(lv_obj_t *cand)
{
    lv_obj_set_style_text_font(cand, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(cand, lv_color_hex(0x0F172A), 0);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cand, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(cand, 1, 0);
    lv_obj_set_style_border_color(cand, lv_color_hex(0xCBD5E1), 0);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(cand, lv_color_hex(0xFFFFFF), LV_PART_ITEMS);
    lv_obj_set_style_text_color(cand, lv_color_hex(0x0F172A), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(cand, lv_color_hex(0xE2E8F0), LV_PART_ITEMS | LV_STATE_PRESSED);
}
#endif

struct ParentChatUiOp {
    ParentChat *app;
    std::vector<ParentChatMsg> *msgs;
    bool initial;
};

ParentChat::ParentChat()
    : ESP_Brookesia_PhoneApp("亲子聊天", &img_app_parent_chat, true)
{
}

ParentChat::~ParentChat() = default;

bool ParentChat::init(void)
{
    parent_chat_bg_poll_start();
    return true;
}

static void format_msg_time(double ts, char *buf, size_t cap)
{
    if (buf == nullptr || cap == 0) {
        return;
    }
    buf[0] = '\0';
    if (ts <= 0) {
        return;
    }
    time_t t = (time_t)ts;
    struct tm tm_out;
    if (localtime_r(&t, &tm_out) == nullptr) {
        return;
    }
    snprintf(buf, cap, "%d月%d日 %02d:%02d", tm_out.tm_mon + 1, tm_out.tm_mday, tm_out.tm_hour, tm_out.tm_min);
}

static void deferred_scroll_bottom(void *p)
{
    ParentChat *app = static_cast<ParentChat *>(p);
    if (app) app->scrollToBottom(false);
}

static void ui_apply_msgs_async(void *p)
{
    ParentChatUiOp *op = static_cast<ParentChatUiOp *>(p);
    if (op == nullptr || op->app == nullptr) {
        delete op;
        return;
    }
    if (op->msgs) {
        op->app->applyServerMessages(*op->msgs, op->initial);
        delete op->msgs;
    }
    delete op;
}

bool ParentChat::hasMessageId(int msg_id) const
{
    if (_msg_col == nullptr || msg_id <= 0) {
        return false;
    }
    const uint32_t n = lv_obj_get_child_cnt(_msg_col);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(_msg_col, i);
        if ((int)(intptr_t)lv_obj_get_user_data(row) == msg_id) {
            return true;
        }
    }
    return false;
}

void ParentChat::appendMessage(int msg_id, const char *sender, const char *body, double created_at)
{
    if (_msg_col == nullptr || body == nullptr || body[0] == '\0') {
        return;
    }
    if (msg_id > 0 && hasMessageId(msg_id)) {
        return;
    }

    const bool mine = (sender != nullptr && strcmp(sender, "child") == 0);

    lv_obj_t *row = lv_obj_create(_msg_col);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(
        row,
        mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START);
    if (msg_id > 0) {
        lv_obj_set_user_data(row, (void *)(intptr_t)msg_id);
    }

    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_width(bubble, lv_pct(78));
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_pad_hor(bubble, 12, 0);
    lv_obj_set_style_pad_ver(bubble, 8, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bubble, 4, 0);
    if (mine) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2563EB), 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, lv_color_hex(0xCBD5E1), 0);
    }

    lv_obj_t *txt = lv_label_create(bubble);
    lv_label_set_text(txt, body);
    lv_obj_set_style_text_font(txt, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(txt, mine ? lv_color_white() : lv_color_hex(0x0F172A), 0);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, lv_pct(100));

    char tbuf[24];
    format_msg_time(created_at, tbuf, sizeof(tbuf));
    if (tbuf[0] != '\0') {
        lv_obj_t *time_lbl = lv_label_create(bubble);
        lv_label_set_text(time_lbl, tbuf);
        lv_obj_set_style_text_font(time_lbl, &lv_font_ui_zh_22, 0);
        lv_obj_set_style_text_color(time_lbl, mine ? lv_color_hex(0xBFDBFE) : lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_opa(time_lbl, LV_OPA_80, 0);
        lv_obj_set_width(time_lbl, lv_pct(100));
        lv_obj_set_style_text_align(time_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    }

    if (_scroll && !_suppress_msg_scroll) {
        lv_obj_scroll_to_view(row, LV_ANIM_ON);
    }
}

void ParentChat::scrollToBottom(bool anim)
{
    if (!_scroll || !_msg_col) return;
    lv_obj_update_layout(_scroll);
    lv_obj_update_layout(_msg_col);
    lv_obj_scroll_to_y(_scroll, LV_COORD_MAX, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

void ParentChat::applyServerMessages(const std::vector<ParentChatMsg> &msgs, bool initial)
{
    if (initial && _msg_col != nullptr) {
        lv_obj_clean(_msg_col);
        _last_id = 0;
    }
    const bool batch = initial && !msgs.empty();
    if (batch) {
        _suppress_msg_scroll = true;
    }
    for (const auto &m : msgs) {
        appendMessage(m.id, m.sender.c_str(), m.body.c_str(), m.created_at);
        if (m.id > _last_id) {
            _last_id = m.id;
        }
    }
    if (batch) {
        _suppress_msg_scroll = false;
        scrollToBottom(false);
        lv_async_call(deferred_scroll_bottom, this);
    } else if (!msgs.empty()) {
        scrollToBottom(true);
    }
}

void ParentChat::refreshFromServer(bool initial)
{
    std::vector<ParentChatMsg> *msgs = new std::vector<ParentChatMsg>();
    ParentChatUnread unread = {};
    bool ok = initial ? parent_chat_fetch_recent(*msgs, &unread) : parent_chat_poll(_last_id, *msgs, &unread);
    if (!ok) {
        delete msgs;
        ESP_LOGW(TAG, "chat fetch failed");
        return;
    }
    if (initial) {
        parent_chat_mark_read_child(unread.latest_id);
    } else if (unread.latest_id > _last_id) {
        parent_chat_mark_read_child(unread.latest_id);
    }
    parent_chat_launcher_set_unread(false);
    ParentChatUiOp *op = new ParentChatUiOp{this, msgs, initial};
    lv_async_call(ui_apply_msgs_async, op);
}

static void initial_load_task(void *p)
{
    ParentChat *app = static_cast<ParentChat *>(p);
    if (app) {
        app->refreshFromServer(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        app->startPoll();
    }
    vTaskDelete(nullptr);
}

static void poll_task(void *p)
{
    ParentChat *app = static_cast<ParentChat *>(p);
    while (app && app->_poll_run) {
        app->refreshFromServer(false);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    app->_poll_task = nullptr;
    vTaskDelete(nullptr);
}

void ParentChat::startPoll(void)
{
    _poll_run = true;
    if (_poll_task == nullptr) {
        xTaskCreate(poll_task, "pchat_poll", 8192, this, 4, &_poll_task);
    }
}

void ParentChat::stopPoll(void)
{
    _poll_run = false;
}

void ParentChat::doSend(void)
{
    if (_input == nullptr) {
        return;
    }
    const char *txt = lv_textarea_get_text(_input);
    if (txt == nullptr || txt[0] == '\0') {
        return;
    }
    char client_id[40];
    snprintf(client_id, sizeof(client_id), "c%lld", (long long)esp_timer_get_time());

    ParentChatMsg sent;
    if (!parent_chat_send(txt, client_id, &sent)) {
        ESP_LOGW(TAG, "send failed");
        return;
    }
    lv_textarea_set_text(_input, "");
    hideInputPanels();
    if (sent.id > 0) {
        const char *show = sent.body.empty() ? txt : sent.body.c_str();
        double ts = sent.created_at > 0 ? sent.created_at : (double)time(nullptr);
        appendMessage(sent.id, "child", show, ts);
        if (sent.id > _last_id) {
            _last_id = sent.id;
        }
    }
}

void ParentChat::updateScrollPadding(void)
{
    if (_scroll == nullptr || _inputStack == nullptr) {
        return;
    }
    /* input/keyboard 与消息区是 flex 兄弟，会自行挤占高度；仅候选栏 IGNORE_LAYOUT 盖住消息底部 */
    lv_coord_t pad = 8;
#if LV_USE_IME_PINYIN
    if (_ime != nullptr) {
        lv_obj_t *cp = lv_ime_pinyin_get_cand_panel(_ime);
        if (cp != nullptr && !lv_obj_has_flag(cp, LV_OBJ_FLAG_HIDDEN)) {
            pad += PCHAT_CAND_H;
        }
    }
#endif
    lv_obj_set_style_pad_bottom(_scroll, pad, 0);
    lv_obj_update_layout(_inputStack);
    lv_obj_update_layout(_scroll);
}

void ParentChat::hideInputPanels(void)
{
    if (_keyboard != nullptr) {
        lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
#if LV_USE_IME_PINYIN
    if (_ime != nullptr) {
        lv_obj_t *cp = lv_ime_pinyin_get_cand_panel(_ime);
        if (cp != nullptr) {
            lv_obj_add_flag(cp, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
    updateScrollPadding();
}

void ParentChat::layoutInputPanels(void)
{
    if (_keyboard != nullptr) {
        lv_obj_clear_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
#if LV_USE_IME_PINYIN
    if (_ime != nullptr && _inputRow != nullptr && _inputStack != nullptr) {
        lv_obj_t *cp = lv_ime_pinyin_get_cand_panel(_ime);
        if (cp != nullptr) {
            lv_obj_set_parent(cp, _inputStack);
            lv_obj_add_flag(cp, LV_OBJ_FLAG_IGNORE_LAYOUT);
            lv_obj_set_width(cp, lv_pct(100));
            lv_obj_set_height(cp, PCHAT_CAND_H);
            lv_obj_update_layout(_inputRow);
            lv_obj_align_to(cp, _inputRow, LV_ALIGN_OUT_TOP_MID, 0, 0);
        }
    }
#endif
    updateScrollPadding();
}

static void deferred_layout_input(void *p)
{
    ParentChat *app = static_cast<ParentChat *>(p);
    if (app) {
        app->layoutInputPanels();
        app->scrollToBottom(false);
    }
}

void ParentChat::onKeyboardEvent(lv_event_t *e)
{
    ParentChat *app = static_cast<ParentChat *>(lv_event_get_user_data(e));
    if (app == nullptr) {
        return;
    }
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_async_call(deferred_layout_input, app);
    }
}

void ParentChat::onTextareaEvent(lv_event_t *e)
{
    ParentChat *app = static_cast<ParentChat *>(lv_event_get_user_data(e));
    if (app == nullptr || app->_keyboard == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(app->_keyboard, app->_input);
        app->layoutInputPanels();
        /* 等 flex 重排后再滚到底，保证最后一条贴在输入框上方 */
        lv_async_call(deferred_scroll_bottom, app);
    } else if (code == LV_EVENT_CANCEL || code == LV_EVENT_DEFOCUSED) {
        app->hideInputPanels();
    }
}

void ParentChat::onSendClicked(lv_event_t *e)
{
    ParentChat *app = static_cast<ParentChat *>(lv_event_get_user_data(e));
    if (app) {
        app->doSend();
    }
}

void ParentChat::buildUi(lv_coord_t vw, lv_coord_t vh)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_size(scr, vw, vh);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xEDEDED), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    _root = lv_obj_create(scr);
    lv_obj_set_size(_root, vw, vh);
    lv_obj_set_style_bg_opa(_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_root, 0, 0);
    lv_obj_set_style_pad_all(_root, 0, 0);
    lv_obj_clear_flag(_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_root, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *head = lv_obj_create(_root);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, 40);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x2563EB), 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(head);
    lv_label_set_text(title, "亲子聊天");
    lv_obj_set_style_text_font(title, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_center(title);

    _scroll = lv_obj_create(_root);
    lv_obj_set_width(_scroll, lv_pct(100));
    lv_obj_set_flex_grow(_scroll, 1);
    lv_obj_set_style_bg_color(_scroll, lv_color_hex(0xEDEDED), 0);
    lv_obj_set_style_border_width(_scroll, 0, 0);
    lv_obj_set_style_pad_all(_scroll, 8, 0);
    lv_obj_add_flag(_scroll, LV_OBJ_FLAG_SCROLLABLE);

    _msg_col = lv_obj_create(_scroll);
    lv_obj_set_width(_msg_col, lv_pct(100));
    lv_obj_set_height(_msg_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(_msg_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(_msg_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_msg_col, 0, 0);
    lv_obj_set_style_pad_row(_msg_col, 10, 0);
    lv_obj_clear_flag(_msg_col, LV_OBJ_FLAG_SCROLLABLE);

    _kb_h = 200;
    if (vh / 3 < _kb_h) {
        _kb_h = vh / 3;
    }

    _inputStack = lv_obj_create(_root);
    lv_obj_set_width(_inputStack, lv_pct(100));
    lv_obj_set_height(_inputStack, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(_inputStack, lv_color_hex(0xF7F7F7), 0);
    lv_obj_set_style_bg_opa(_inputStack, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_inputStack, 1, 0);
    lv_obj_set_style_border_side(_inputStack, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(_inputStack, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_pad_all(_inputStack, 0, 0);
    lv_obj_clear_flag(_inputStack, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(_inputStack, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(_inputStack, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *input_row = lv_obj_create(_inputStack);
    _inputRow = input_row;
    lv_obj_set_width(input_row, lv_pct(100));
    lv_obj_set_height(input_row, PCHAT_INPUT_ROW_H);
    lv_obj_set_style_bg_opa(input_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(input_row, 0, 0);
    lv_obj_set_style_pad_all(input_row, 8, 0);
    lv_obj_clear_flag(input_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(input_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(input_row, 8, 0);

    _input = lv_textarea_create(input_row);
    lv_obj_set_flex_grow(_input, 1);
    lv_obj_set_height(_input, 40);
    lv_textarea_set_one_line(_input, true);
    lv_textarea_set_max_length(_input, 500);
    lv_textarea_set_placeholder_text(_input, "输入消息");
    lv_obj_set_style_text_font(_input, &lv_font_ui_zh_22, 0);
    lv_obj_add_event_cb(_input, onTextareaEvent, LV_EVENT_FOCUSED, this);
    lv_obj_add_event_cb(_input, onTextareaEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(_input, onTextareaEvent, LV_EVENT_CANCEL, this);
    lv_obj_add_event_cb(_input, onTextareaEvent, LV_EVENT_DEFOCUSED, this);

    lv_obj_t *send_btn = lv_btn_create(input_row);
    lv_obj_set_size(send_btn, 80, 40);
    lv_obj_set_style_bg_color(send_btn, lv_color_hex(0x2563EB), 0);
    lv_obj_t *send_lbl = lv_label_create(send_btn);
    lv_label_set_text(send_lbl, "发送");
    lv_obj_set_style_text_font(send_lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(send_lbl, lv_color_white(), 0);
    lv_obj_center(send_lbl);
    lv_obj_add_event_cb(send_btn, onSendClicked, LV_EVENT_CLICKED, this);

    _keyboard = lv_keyboard_create(_inputStack);
    lv_obj_set_width(_keyboard, lv_pct(100));
    lv_obj_set_height(_keyboard, _kb_h);
    lv_keyboard_set_mode(_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_add_flag(_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(_keyboard, _input);

#if LV_USE_IME_PINYIN
    _ime = lv_ime_pinyin_create(_root);
    lv_obj_add_flag(_ime, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(_ime, &lv_font_ui_zh_22, 0);
    lv_ime_pinyin_set_keyboard(_ime, _keyboard);
    lv_ime_pinyin_set_dict(_ime, (lv_pinyin_dict_t *)parent_chat_pinyin_dict);
    lv_obj_t *cand = lv_ime_pinyin_get_cand_panel(_ime);
    lv_obj_set_parent(cand, _inputStack);
    lv_obj_add_flag(cand, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_width(cand, lv_pct(100));
    lv_obj_set_height(cand, PCHAT_CAND_H);
    styleCandPanel(cand);
    lv_obj_align_to(cand, _inputRow, LV_ALIGN_OUT_TOP_MID, 0, 0);
    lv_obj_add_flag(cand, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_add_event_cb(_keyboard, onKeyboardEvent, LV_EVENT_VALUE_CHANGED, this);

    updateScrollPadding();
}

bool ParentChat::run(void)
{
    lv_area_t area = getVisualArea();
    const lv_coord_t vw = area.x2 - area.x1;
    const lv_coord_t vh = area.y2 - area.y1;
    _last_id = 0;
    buildUi(vw, vh);
    parent_chat_launcher_set_unread(false);
    xTaskCreate(initial_load_task, "pchat_load", 8192, this, 4, nullptr);
    return true;
}

bool ParentChat::pause(void)
{
    stopPoll();
    return true;
}

bool ParentChat::resume(void)
{
    startPoll();
    scrollToBottom(false);
    return true;
}

bool ParentChat::back(void)
{
    notifyCoreClosed();
    return true;
}

bool ParentChat::close(void)
{
    stopPoll();
    _root = nullptr;
    _scroll = nullptr;
    _msg_col = nullptr;
    _input = nullptr;
    _inputRow = nullptr;
    _inputStack = nullptr;
    _keyboard = nullptr;
    _ime = nullptr;
    return true;
}
