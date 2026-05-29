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

#define PARENT_PRINT_MAX_FILE   (2 * 1024 * 1024)
#define PARENT_PRINT_PART_BYTES 4096
#define PARENT_PRINT_PATH_MAX   128

bool parent_print_fs_is_mounted(void);

bool parent_print_fs_has_space_for(size_t file_bytes);

esp_err_t parent_print_fs_make_part_path(int print_id, char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
