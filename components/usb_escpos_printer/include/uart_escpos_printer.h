/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install UART driver for ESC/POS over TTL (same bytes as USB/BLE paths).
 *
 * Default pins: TX GPIO4 → printer RX, RX GPIO5 ← printer TX (menuconfig overrides).
 * Uses HP UART (default port 2 on ESP32-P4 to avoid UART1 / I2S pin clash), 8N1, no flow control.
 */
esp_err_t uart_escpos_printer_init(void);

/** Tear down and reinstall UART (use after display/BSP init if link was silent). */
esp_err_t uart_escpos_printer_reinit(void);

esp_err_t uart_escpos_printer_deinit(void);

/** @return true if UART was installed successfully */
bool uart_escpos_printer_ready(void);

esp_err_t uart_escpos_print_jpeg_file(const char *filepath);

/** Raw ESC/POS bytes over the same UART (thread-safe vs jpeg print). */
esp_err_t uart_escpos_printer_send_bytes(const uint8_t *data, size_t len);

/** ASCII smoke test: HELLO EM5820H + feed (simplest wiring check). */
esp_err_t uart_escpos_printer_test_hello(void);

/** ESC/POS: init, Font B (9×17), print "123", feed — for wiring check. */
esp_err_t uart_escpos_printer_test_small_123(void);

/** Demo/1 style: center, bold, underline, 2×2, GBK「东为打印机」, feed 10 lines. */
esp_err_t uart_escpos_printer_test_demo_format(void);

#ifdef __cplusplus
}
#endif
