/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "escpos_jpeg_raster.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Print UTF-8 plain text as ESC/POS strip (Font B, GBK, left align, feed).
 * Wraps to menuconfig column width for 58mm paper.
 */
esp_err_t escpos_print_utf8_strip(const char *utf8, escpos_link_write_fn write_fn, void *ctx);

/** Prefer UART, else USB; returns ESP_ERR_NOT_FOUND if no printer. */
esp_err_t escpos_printer_print_utf8(const char *utf8);

#ifdef __cplusplus
}
#endif
