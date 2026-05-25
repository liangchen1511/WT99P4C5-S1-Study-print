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
    bool back(void) override;
    bool close(void) override;
    bool pause(void) override;
    bool resume(void) override;

    bool init(void) override;

private:
    lv_obj_t *_root = nullptr;
};
