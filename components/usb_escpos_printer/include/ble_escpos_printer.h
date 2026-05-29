/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After WiFi / esp_hosted transport is up; starts NimBLE + scan for BLE UART printer. */
esp_err_t ble_escpos_printer_init(void);

/** Nordic UART RX characteristic discovered and link writable. */
bool ble_escpos_printer_ready(void);

/** Same raster pipeline as USB path; sends ESC/POS over BLE writes (chunked). */
esp_err_t ble_escpos_print_jpeg_file(const char *filepath);

#ifdef __cplusplus
}
#endif
