/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARENT_ALBUM_FILENAME_MAX 64
#define PARENT_ALBUM_PATH_MAX     128
#define PARENT_ALBUM_MAX_FILE     (4 * 1024 * 1024)
#define PARENT_ALBUM_RESERVE      (2 * 1024 * 1024)
#define PARENT_ALBUM_PART_BYTES   4096

bool parent_album_fs_is_mounted(void);

bool parent_album_fs_has_space_for(size_t file_bytes);

bool parent_album_fs_sanitize_name(const char *in, char *out, size_t out_cap);

esp_err_t parent_album_fs_pick_final_path(const char *basename_or_null, char *out, size_t out_cap);

esp_err_t parent_album_fs_make_part_path(int album_id, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
