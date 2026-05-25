/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/**
 * BLE 热敏打印（NimBLE 中心 + ESP-Hosted VHCI）：0 = 完全关闭，不调用 ble_escpos_printer_init()，
 * 避免与当前调试目标冲突；需要时再改为 1，并保证 sdkconfig 中 CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y。
 *
 * TTL 串口打印可参考乐鑫 ESP-IDF 例程：examples/peripherals/uart/uart_echo、uart_events
 *（uart_driver_install → uart_param_config → uart_set_pin 顺序与波特率设置）。
 */
#ifndef WT_USE_BLE_ESC_POS_PRINTER
#define WT_USE_BLE_ESC_POS_PRINTER 0
#endif
