/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Print.hpp"
#include "lv_font_ui_zh.h"
#include "parent_guard.hpp"

LV_IMG_DECLARE(img_app_print);

Print::Print()
    : ESP_Brookesia_PhoneApp("打印", &img_app_print, true)
{
}

Print::~Print() = default;

bool Print::init(void)
{
    return true;
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
    lv_obj_set_style_pad_all(_root, 24, 0);

    lv_obj_t *lbl = lv_label_create(_root);
    lv_label_set_text(lbl,
                      "打印入口：SoTi 答案页「仅题目 / 含答案」。\n"
                      "USB：热敏接 USB-A 主机口。\n"
                      "TTL：J6 GPIO4→打印机RX，GPIO5←打印机TX，GND 共地；115200 8N1。\n"
                      "EM5820H 常需 CTS：GPIO1(J6 IO1)接打印机 CTS，或 CTS 接 3.3V。\n"
                      "无声可试 9600 或 menuconfig 开 SWAP_TX_RX。");
    lv_obj_set_style_text_font(lbl, &lv_font_ui_zh_22, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, vw - 48);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 0);

    return true;
}

bool Print::pause(void)
{
    return true;
}

bool Print::resume(void)
{
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
    return true;
}
