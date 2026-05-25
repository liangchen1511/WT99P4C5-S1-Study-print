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

/**
 * @brief Install USB Host client task (requires bsp_usb_host_start() already called).
 *
 * Supports ESC/POS over USB Bulk OUT on:
 * - CDC ACM **Data** interface (0x0A) — typical “USB 串口”热敏（如优库 EM5820H），连接后下发 9600 8N1 + DTR/RTS；
 * - USB Printer class (0x07) 或 Vendor (0xFF) Bulk OUT。
 *
 * Virtual COM on PC still uses the device’s Bulk OUT pipe; no UART wiring required when using the board USB‑A host port.
 *
 * CDC 波特率在 menuconfig「USB ESC/POS printer」里配置（默认 9600，与 EM5820H 一致）；若模组已改为 115200，改为 115200 即可。
 */
esp_err_t usb_escpos_printer_init(void);

/** @return true if a supported printer is connected and claimed */
bool usb_escpos_printer_ready(void);

/**
 * @brief Print a baseline JPEG from filesystem (SD path).
 *
 * Decodes with esp_jpeg, scales to max width @ref usb_escpos_printer_max_width(),
 * dithers to 1‑bit and sends GS v 0 raster plus a short text header (ASCII filename).
 *
 * Thread-safe vs USB stack when used together with the internal client task.
 */
esp_err_t usb_escpos_print_jpeg_file(const char *filepath);

/** Printable width in pixels for ESC/POS packing (default 384, typical 58 mm head). */
unsigned usb_escpos_printer_max_width(void);

/** Raw ESC/POS bytes (thread-safe vs USB client task). */
esp_err_t usb_escpos_printer_send_bytes(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
