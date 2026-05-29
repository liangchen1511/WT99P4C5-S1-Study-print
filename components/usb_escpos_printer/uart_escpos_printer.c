/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESC/POS over UART (TTL): GPIO4 TX → printer RX, GPIO5 RX ← printer TX.
 * Printer silkscreen CTS = RTS busy out -> ESP GPIO6 UART CTS + HW flow control.
 */

#include "sdkconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"

#include "uart_escpos_printer.h"
#include "escpos_jpeg_raster.h"

static const char *TAG = "uart_escpos";

#ifndef CONFIG_UART_ESC_POS_BAUD
#define CONFIG_UART_ESC_POS_BAUD 115200
#endif
#ifndef CONFIG_UART_ESC_POS_TX_GPIO
#define CONFIG_UART_ESC_POS_TX_GPIO 4
#endif
#ifndef CONFIG_UART_ESC_POS_RX_GPIO
#define CONFIG_UART_ESC_POS_RX_GPIO 5
#endif
#ifndef CONFIG_UART_ESC_POS_PORT
#define CONFIG_UART_ESC_POS_PORT 2
#endif
#ifndef CONFIG_UART_ESC_POS_FLOW_CTS_GPIO
#define CONFIG_UART_ESC_POS_FLOW_CTS_GPIO -1
#endif

#if CONFIG_UART_ESC_POS_PORT == 0
#define UART_ESC_UART_NUM UART_NUM_0
#elif CONFIG_UART_ESC_POS_PORT == 1
#define UART_ESC_UART_NUM UART_NUM_1
#elif CONFIG_UART_ESC_POS_PORT == 2
#define UART_ESC_UART_NUM UART_NUM_2
#elif CONFIG_UART_ESC_POS_PORT == 3
#define UART_ESC_UART_NUM UART_NUM_3
#elif CONFIG_UART_ESC_POS_PORT == 4
#define UART_ESC_UART_NUM UART_NUM_4
#else
#define UART_ESC_UART_NUM UART_NUM_2
#endif

#define UART_TX_BUF_SIZE  4096
#define UART_RX_BUF_SIZE  256

static SemaphoreHandle_t s_wr_mux;
static bool s_inited_ok;

static int s_tx_gpio;
static int s_rx_gpio;
static int s_cts_gpio;

static esp_err_t uart_escpos_send_bytes_locked(const uint8_t *data, size_t len, int *bytes_written)
{
    if (!s_inited_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    size_t off = 0;
    int total = 0;
    while (off < len) {
        int w = uart_write_bytes(UART_ESC_UART_NUM, data + off, len - off);
        if (w < 0) {
            ret = ESP_FAIL;
            break;
        }
        total += w;
        off += (size_t)w;
    }
    if (ret == ESP_OK) {
        uint32_t ms = 5000 + (uint32_t)((len * 10000ULL) / (unsigned)CONFIG_UART_ESC_POS_BAUD);
        if (ms > 60000) {
            ms = 60000;
        }
        ret = uart_wait_tx_done(UART_ESC_UART_NUM, pdMS_TO_TICKS(ms));
    }
    if (bytes_written != NULL) {
        *bytes_written = total;
    }
    return ret;
}

static void uart_escpos_log_tx_hex(const char *label, const uint8_t *data, size_t len)
{
    char line[96];
    size_t show = len < 16 ? len : 16;
    size_t pos = 0;
    for (size_t i = 0; i < show && pos + 3 < sizeof(line); i++) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%02X ", data[i]);
    }
    ESP_LOGI(TAG, "%s: %u bytes, first: %s%s", label, (unsigned)len, line, len > show ? "..." : "");
}

static esp_err_t uart_escpos_hw_install(void)
{
#if CONFIG_UART_ESC_POS_SWAP_TX_RX
    s_tx_gpio = CONFIG_UART_ESC_POS_RX_GPIO;
    s_rx_gpio = CONFIG_UART_ESC_POS_TX_GPIO;
    ESP_LOGW(TAG, "SWAP_TX_RX: using TX=GPIO%d RX=GPIO%d", s_tx_gpio, s_rx_gpio);
#else
    s_tx_gpio = CONFIG_UART_ESC_POS_TX_GPIO;
    s_rx_gpio = CONFIG_UART_ESC_POS_RX_GPIO;
#endif
#if CONFIG_UART_ESC_POS_FLOW_CTS_GPIO >= 0
    s_cts_gpio = CONFIG_UART_ESC_POS_FLOW_CTS_GPIO;
#else
    s_cts_gpio = -1;
#endif

    if (s_tx_gpio >= 0) {
        gpio_reset_pin((gpio_num_t)s_tx_gpio);
    }
    if (s_rx_gpio >= 0) {
        gpio_reset_pin((gpio_num_t)s_rx_gpio);
    }
    if (s_cts_gpio >= 0) {
        gpio_reset_pin((gpio_num_t)s_cts_gpio);
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_UART_ESC_POS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = (s_cts_gpio >= 0) ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(UART_ESC_UART_NUM, UART_RX_BUF_SIZE, UART_TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install UART%d: %s", (int)UART_ESC_UART_NUM, esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(UART_ESC_UART_NUM, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config: %s", esp_err_to_name(err));
        uart_driver_delete(UART_ESC_UART_NUM);
        return err;
    }

    err = uart_set_pin(UART_ESC_UART_NUM, s_tx_gpio, s_rx_gpio, UART_PIN_NO_CHANGE, s_cts_gpio >= 0 ? s_cts_gpio : UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin TX=%d RX=%d CTS=%d: %s", s_tx_gpio, s_rx_gpio, s_cts_gpio,
                 esp_err_to_name(err));
        uart_driver_delete(UART_ESC_UART_NUM);
        return err;
    }

    if (s_rx_gpio >= 0) {
        gpio_set_pull_mode((gpio_num_t)s_rx_gpio, GPIO_PULLUP_ONLY);
    }
    if (s_cts_gpio >= 0) {
        gpio_set_pull_mode((gpio_num_t)s_cts_gpio, GPIO_PULLUP_ONLY);
        ESP_LOGI(TAG, "CTS GPIO%d level=%d (1=idle 0=busy)", s_cts_gpio, gpio_get_level((gpio_num_t)s_cts_gpio));
    }
    if (s_tx_gpio >= 0) {
        gpio_set_drive_capability((gpio_num_t)s_tx_gpio, GPIO_DRIVE_CAP_3);
    }

    uart_flush_input(UART_ESC_UART_NUM);
    return ESP_OK;
}

esp_err_t uart_escpos_printer_deinit(void)
{
    if (!s_inited_ok) {
        return ESP_OK;
    }
    s_inited_ok = false;
    esp_err_t err = uart_driver_delete(UART_ESC_UART_NUM);
    ESP_LOGI(TAG, "UART deinit: %s", esp_err_to_name(err));
    return err;
}

/** Install driver + wake. Caller must hold s_wr_mux if concurrent access is possible. */
static esp_err_t uart_escpos_install_locked(void)
{
    esp_err_t err = uart_escpos_hw_install();
    if (err != ESP_OK) {
        return err;
    }

    s_inited_ok = true;
    ESP_LOGI(TAG,
             "UART ESC/POS: port=%d UART%d TX=GPIO%d RX=GPIO%d CTS=GPIO%d flow=%s %d 8N1",
             CONFIG_UART_ESC_POS_PORT, (int)UART_ESC_UART_NUM, s_tx_gpio, s_rx_gpio, s_cts_gpio,
             s_cts_gpio >= 0 ? "CTS" : "none", CONFIG_UART_ESC_POS_BAUD);

    const uint8_t wake[] = {0x1B, 0x40, '\r', '\n'};
    int written = 0;
    (void)uart_escpos_send_bytes_locked(wake, sizeof(wake), &written);
    ESP_LOGI(TAG, "wake ESC @ wrote %d bytes", written);

    return ESP_OK;
}

esp_err_t uart_escpos_printer_init(void)
{
    if (s_wr_mux == NULL) {
        s_wr_mux = xSemaphoreCreateMutex();
        if (s_wr_mux == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_wr_mux, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!s_inited_ok) {
        err = uart_escpos_install_locked();
    }
    xSemaphoreGive(s_wr_mux);
    return err;
}

esp_err_t uart_escpos_printer_reinit(void)
{
    if (s_wr_mux == NULL) {
        return uart_escpos_printer_init();
    }

    xSemaphoreTake(s_wr_mux, portMAX_DELAY);
    (void)uart_escpos_printer_deinit();
    esp_err_t err = uart_escpos_install_locked();
    xSemaphoreGive(s_wr_mux);
    return err;
}

bool uart_escpos_printer_ready(void)
{
    return s_inited_ok;
}

static esp_err_t uart_escpos_link_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!s_inited_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_wr_mux, portMAX_DELAY);
    esp_err_t ret = uart_escpos_send_bytes_locked(data, len, NULL);
    xSemaphoreGive(s_wr_mux);
    return ret;
}

esp_err_t uart_escpos_printer_send_bytes(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_wr_mux, portMAX_DELAY);
    int written = 0;
    esp_err_t ret = uart_escpos_send_bytes_locked(data, len, &written);
    xSemaphoreGive(s_wr_mux);
    if (ret == ESP_OK && (size_t)written != len) {
        ESP_LOGW(TAG, "short write: %d/%u", written, (unsigned)len);
        ret = ESP_FAIL;
    }
    return ret;
}

esp_err_t uart_escpos_printer_test_hello(void)
{
    static const uint8_t seq[] = {
        0x1B, 0x40,
        'H', 'E', 'L', 'L', 'O', ' ', 'E', 'M', '5', '8', '2', '0', 'H',
        '\r', '\n',
        0x1B, 0x64, 0x03,
    };
    uart_escpos_log_tx_hex("test_hello", seq, sizeof(seq));
    esp_err_t err = uart_escpos_printer_send_bytes(seq, sizeof(seq));
    ESP_LOGI(TAG, "test_hello err=%s", esp_err_to_name(err));
    return err;
}

esp_err_t uart_escpos_printer_test_small_123(void)
{
    static const uint8_t seq[] = {
        0x1B, 0x40,
        0x1B, 0x4D, 0x01,
        '1', '2', '3', '\r', '\n',
        0x1B, 0x64, 0x05,
    };
    uart_escpos_log_tx_hex("test_small_123", seq, sizeof(seq));
    esp_err_t err = uart_escpos_printer_send_bytes(seq, sizeof(seq));
    ESP_LOGI(TAG, "test_small_123 err=%s", esp_err_to_name(err));
    return err;
}

esp_err_t uart_escpos_printer_test_demo_format(void)
{
    static const uint8_t seq[] = {
        0x1B, 0x40,
        0x1B, 0x61, 0x01,
        0x1B, 0x45, 0x01,
        0x1B, 0x2D, 0x02,
        0x1D, 0x21, 0x11,
        0x1C, 0x26,
        0xB6, 0xAB, 0xCE, 0xAA, 0xB4, 0xF2, 0xD3, 0xA1,
        '\r', '\n',
        0x1B, 0x64, 0x0A,
    };
    uart_escpos_log_tx_hex("test_demo_format", seq, sizeof(seq));
    esp_err_t err = uart_escpos_printer_send_bytes(seq, sizeof(seq));
    ESP_LOGI(TAG, "test_demo_format err=%s", esp_err_to_name(err));
    return err;
}

esp_err_t uart_escpos_print_jpeg_file(const char *filepath)
{
    if (filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    return escpos_jpeg_raster_print(filepath, uart_escpos_link_write, NULL);
}

int uart_escpos_cts_gpio_level(void)
{
    if (s_cts_gpio < 0) {
        return -1;
    }
    return gpio_get_level((gpio_num_t)s_cts_gpio);
}
