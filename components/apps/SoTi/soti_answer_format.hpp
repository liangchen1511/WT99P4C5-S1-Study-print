/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

/**
 * 将豆包返回的 Markdown/LaTeX 转为设备屏可读的纯文本（去命令、保留算式与中文）。
 * 返回堆内存字符串，调用方 free()；失败返回 nullptr。
 */
char *soti_format_answer_for_display(const char *raw);
