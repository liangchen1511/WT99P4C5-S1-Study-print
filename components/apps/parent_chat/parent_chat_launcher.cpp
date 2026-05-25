/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_chat_launcher.hpp"

#include "lvgl.h"

LV_IMG_DECLARE(img_app_parent_chat);

static volatile bool s_unread_pending;
static volatile bool s_unread_value;

static lv_obj_t *find_parent_chat_icon_img(lv_obj_t *root, int depth)
{
    if (root == nullptr || depth > 12) {
        return nullptr;
    }
    if (!lv_obj_is_valid(root)) {
        return nullptr;
    }
    uint32_t n = lv_obj_get_child_cnt(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(root, i);
        if (ch == nullptr || !lv_obj_is_valid(ch)) {
            continue;
        }
        if (lv_obj_check_type(ch, &lv_img_class)) {
            const void *src = lv_img_get_src(ch);
            if (src == &img_app_parent_chat) {
                return ch;
            }
        }
        lv_obj_t *found = find_parent_chat_icon_img(ch, depth + 1);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

static void apply_unread_on_lvgl(void *ud)
{
    (void)ud;
    if (!s_unread_pending) {
        return;
    }
    s_unread_pending = false;
    const bool has_unread = s_unread_value;

    lv_obj_t *scr = lv_scr_act();
    if (scr == nullptr || !lv_obj_is_valid(scr)) {
        return;
    }
    lv_obj_t *img = find_parent_chat_icon_img(scr, 0);
    if (img == nullptr || !lv_obj_is_valid(img)) {
        return;
    }
    if (has_unread) {
        lv_obj_set_style_img_recolor(img, lv_color_hex(0xEF4444), 0);
        lv_obj_set_style_img_recolor_opa(img, LV_OPA_70, 0);
    } else {
        lv_obj_set_style_img_recolor_opa(img, LV_OPA_0, 0);
    }
}

void parent_chat_launcher_set_unread(bool has_unread)
{
    s_unread_value = has_unread;
    s_unread_pending = true;
    (void)lv_async_call(apply_unread_on_lvgl, nullptr);
}
