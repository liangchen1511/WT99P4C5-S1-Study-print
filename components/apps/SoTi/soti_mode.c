/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soti_mode.h"

static const char *const k_display[] = {
    "搜题",
    "拍照翻译",
    "错题讲解",
    "作业批改",
    "文档摘要",
    "每日一句",
};

static const char *const k_loading[] = {
    "搜题识别中",
    "拍照翻译中",
    "错题讲解中",
    "作业批改中",
    "文档摘要中",
    "每日一句",
};

static const char k_roller_opts[] = "搜题\n拍照翻译\n错题讲解\n作业批改\n文档摘要\n每日一句";

const char *soti_mode_query_value(soti_mode_t mode)
{
    switch (mode) {
    case SOTI_MODE_SOLVE:
        return "solve";
    case SOTI_MODE_TRANSLATE:
        return "translate";
    case SOTI_MODE_TUTOR:
        return "tutor";
    case SOTI_MODE_GRADE:
        return "grade";
    case SOTI_MODE_SUMMARY:
        return "summary";
    case SOTI_MODE_DAILY:
        return "daily";
    default:
        return "solve";
    }
}

const char *soti_mode_display_name(soti_mode_t mode)
{
    if ((unsigned)mode < (unsigned)SOTI_MODE_COUNT) {
        return k_display[mode];
    }
    return k_display[0];
}

const char *soti_mode_loading_hint(soti_mode_t mode)
{
    if ((unsigned)mode < (unsigned)SOTI_MODE_COUNT) {
        return k_loading[mode];
    }
    return k_loading[0];
}

const char *soti_roller_options(void)
{
    return k_roller_opts;
}

bool soti_mode_uses_camera(soti_mode_t mode)
{
    return mode != SOTI_MODE_DAILY;
}
