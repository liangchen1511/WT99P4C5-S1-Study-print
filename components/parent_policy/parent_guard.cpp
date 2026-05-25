/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_guard.hpp"
#include "parent_policy.hpp"

#include "lv_font_ui_zh.h"
#include "lvgl.h"

bool parent_guard_app_run(const char *app_id)
{
    if (parent_policy_is_app_allowed(app_id)) {
        return true;
    }
    const char *reason = parent_policy_block_reason();
    lv_obj_t *mbox = lv_msgbox_create(nullptr, "学习守护", reason, nullptr, true);
    if (mbox != nullptr) {
        lv_obj_center(mbox);
        lv_obj_t *title = lv_msgbox_get_title(mbox);
        lv_obj_t *text = lv_msgbox_get_text(mbox);
        if (title != nullptr) {
            lv_obj_set_style_text_font(title, &lv_font_ui_zh_22, 0);
        }
        if (text != nullptr) {
            lv_obj_set_style_text_font(text, &lv_font_ui_zh_22, 0);
        }
    }
    return false;
}
