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

#define PARENT_PRINT_JOB_TYPE_IMAGE 0
#define PARENT_PRINT_JOB_TYPE_TEXT  1

typedef struct {
    int id;
    int type;
    char name[64];
    char text_title[64];
    size_t size;
    double created_at;
} parent_print_job_t;

typedef void (*parent_print_sync_status_fn_t)(const char *line, void *user_data);

void parent_print_sync_set_status_callback(parent_print_sync_status_fn_t fn, void *user_data);

uint32_t parent_print_sync_pending_count(void);

esp_err_t parent_print_sync_refresh_pending(void);

size_t parent_print_sync_job_count(void);

bool parent_print_sync_get_job(size_t index, parent_print_job_t *out);

esp_err_t parent_print_sync_print_job(int job_id);

bool parent_print_sync_is_busy(void);

void parent_print_sync_bg_start(void);

void parent_print_sync_bg_stop(void);

void parent_print_sync_pause(bool paused);

#ifdef __cplusplus
}
#endif
