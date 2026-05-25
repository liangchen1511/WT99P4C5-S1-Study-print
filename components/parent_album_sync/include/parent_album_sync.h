/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_enter)(void);
    void (*on_exit)(void);
} parent_album_sync_hooks_t;

typedef void (*parent_album_sync_status_fn_t)(const char *line, void *user_data);

void parent_album_sync_set_hooks(const parent_album_sync_hooks_t *hooks);

void parent_album_sync_set_status_callback(parent_album_sync_status_fn_t fn, void *user_data);

/** Pending count from last poll (0 if unknown). */
uint32_t parent_album_sync_pending_count(void);

/** HTTP poll only; updates pending count for UI (non-blocking if called from worker). */
esp_err_t parent_album_sync_refresh_pending(void);

/** Poll server and download all pending JPEGs to /sdcard. */
esp_err_t parent_album_sync_run_once(void);

void parent_album_sync_bg_start(void);

void parent_album_sync_bg_stop(void);

/** 搜题上传等大块 HTTP 期间暂停后台 poll/下载，避免与 SDIO WiFi 抢 mempool。 */
void parent_album_sync_pause(bool paused);

#ifdef __cplusplus
}
#endif
