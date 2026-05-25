/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 与服务器 ?mode= 一致（daily 走 POST /upload?mode=daily，不经 JPEG）。 */
typedef enum {
    SOTI_MODE_SOLVE = 0,
    SOTI_MODE_TRANSLATE,
    SOTI_MODE_TUTOR,
    SOTI_MODE_GRADE,
    SOTI_MODE_SUMMARY,
    /** 滚轮/快门专用：走 POST /upload?mode=daily，不拍照 */
    SOTI_MODE_DAILY,
    SOTI_MODE_COUNT,
} soti_mode_t;

const char *soti_mode_query_value(soti_mode_t mode);
const char *soti_mode_display_name(soti_mode_t mode);
const char *soti_mode_loading_hint(soti_mode_t mode);
const char *soti_roller_options(void);
bool soti_mode_uses_camera(soti_mode_t mode);

#ifdef __cplusplus
}
#endif
