/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "soti_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOTI_PRINT_QUESTION_MAX     4096
#define SOTI_PRINT_WITH_ANSWER_MAX  8192

typedef struct {
    char question[SOTI_PRINT_QUESTION_MAX];
    char with_answer[SOTI_PRINT_WITH_ANSWER_MAX];
    bool has_question;
    bool has_with_answer;
} soti_print_sections_t;

void soti_print_sections_clear(soti_print_sections_t *out);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/** Split formatted display text into print strips (mirrors server split_print_sections). */
void soti_split_print_sections(const char *display_utf8, soti_mode_t mode, soti_print_sections_t *out);

struct cJSON;
void soti_print_sections_from_json(const cJSON *print_obj, soti_print_sections_t *out);

#endif
