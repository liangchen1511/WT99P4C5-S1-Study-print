/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lvgl.h"
#include "esp_brookesia.hpp"

class Print : public ESP_Brookesia_PhoneApp {
public:
    Print();
    ~Print();

    bool run(void) override;
    bool pause(void) override;
    bool resume(void) override;
    bool back(void) override;
    bool close(void) override;
    bool init(void) override;

    void set_status(const char *msg);
    void set_printing(bool on);
    void rebuild_job_list(void);
    void request_refresh(void);

private:
    static void on_refresh_clicked(lv_event_t *e);
    static void on_print_clicked(lv_event_t *e);
    static void print_status_cb(const char *line, void *user);

    lv_obj_t *_root = nullptr;
    lv_obj_t *_lbl_status = nullptr;
    lv_obj_t *_list = nullptr;
    lv_obj_t *_lbl_hint = nullptr;
    bool _printing = false;
};
