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

/** 启动后台拉取任务（WiFi 可用时） */
void parent_policy_init(void);

/** 是否允许启动 app_id（game_2048 / video / music 等） */
bool parent_policy_is_app_allowed(const char *app_id);

/** 人类可读拒绝原因（静态缓冲，勿长期保存指针） */
const char *parent_policy_block_reason(void);

/** 暂停/恢复后台 policy 轮询（如 SD 网页传图专注模式） */
void parent_policy_poll_pause(bool paused);

#ifdef __cplusplus
}
#endif
