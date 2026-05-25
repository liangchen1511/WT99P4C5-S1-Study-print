/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Transport hook: same contract as Dongwei printer SDK `send_init` (raw bytes to device). */
typedef esp_err_t (*escpos_link_write_fn)(void *ctx, const uint8_t *data, size_t len);

unsigned escpos_jpeg_raster_max_width(void);

/**
 * Decode JPEG from path, rasterize to ESC/POS GS v 0, send via @p write_fn.
 * @param filepath Full path (e.g. SD mount + filename).
 */
esp_err_t escpos_jpeg_raster_print(const char *filepath, escpos_link_write_fn write_fn, void *ctx);

#ifdef __cplusplus
}
#endif
