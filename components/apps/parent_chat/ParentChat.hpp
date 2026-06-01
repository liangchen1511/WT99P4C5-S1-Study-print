/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "parent_chat_api.hpp"
#include "systems/phone/esp_brookesia_phone_app.hpp"

class ParentChat : public ESP_Brookesia_PhoneApp {
public:
    ParentChat();
    ~ParentChat() override;

    bool init(void) override;
    bool run(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool back(void) override;
    bool close(void) override;

    bool hasMessageId(int msg_id) const;
    void appendMessage(int msg_id, const char *sender, const char *body, double created_at);
    void applyServerMessages(const std::vector<ParentChatMsg> &msgs, bool initial);
    void refreshFromServer(bool initial);
    void startPoll(void);
    void stopPoll(void);
    void doSend(void);
    void hideInputPanels(void);
    void layoutInputPanels(void);
    void scrollToBottom(bool anim);
    void updateScrollPadding(void);

    int _last_id = 0;
    volatile bool _poll_run = false;
    TaskHandle_t _poll_task = nullptr;

private:
    void buildUi(lv_coord_t vw, lv_coord_t vh);
    static void onTextareaEvent(lv_event_t *e);
    static void onSendClicked(lv_event_t *e);
    static void onKeyboardEvent(lv_event_t *e);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_scroll = nullptr;
    lv_obj_t *_msg_col = nullptr;
    lv_obj_t *_input = nullptr;
    lv_obj_t *_inputRow = nullptr;
    lv_obj_t *_inputStack = nullptr;
    lv_obj_t *_keyboard = nullptr;
    lv_obj_t *_ime = nullptr;
    lv_coord_t _kb_h = 200;
    bool _suppress_msg_scroll = false;
};
