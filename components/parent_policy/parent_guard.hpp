/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus

#include "lvgl.h"

/** 若不允许启动则弹窗并返回 false */
bool parent_guard_app_run(const char *app_id);

#endif
