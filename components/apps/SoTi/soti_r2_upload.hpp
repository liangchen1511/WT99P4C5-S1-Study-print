/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "soti_print_sections.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * POST raw JPEG to Worker. Caller owns jpeg.
 * If answer_out is not NULL, response body is parsed (JSON field "answer" or "error");
 * on parse failure a truncated raw body is copied. UTF-8 safe if server sends valid UTF-8 JSON.
 * @param print_out optional; filled from JSON "print" object when present
 */
/**
 * @param mode 服务器 ?mode=：solve | translate | tutor | grade | summary；NULL 等同 solve
 */
esp_err_t soti_upload_jpeg_to_worker(
    const uint8_t *jpeg,
    size_t jpeg_len,
    char *answer_out,
    size_t answer_out_sz,
    const char *mode,
    soti_print_sections_t *print_out);

/** GET /upload/daily — A5 每日一句（无需 JPEG） */
esp_err_t soti_fetch_daily_line(char *answer_out, size_t answer_out_sz, soti_print_sections_t *print_out);

#ifdef __cplusplus
}
#endif
