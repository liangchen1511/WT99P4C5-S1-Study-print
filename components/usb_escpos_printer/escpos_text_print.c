/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "escpos_text_print.h"
#include "uart_escpos_printer.h"
#include "usb_escpos_printer.h"

static const char *TAG = "escpos_text";

#ifndef CONFIG_UART_ESC_POS_PRINT_COLS
#define CONFIG_UART_ESC_POS_PRINT_COLS 42
#endif

#include "utf8_gbk_map.inc"

static int utf8_decode_one(const char *s, size_t len, uint32_t *cp_out, size_t *used_out)
{
    if (len == 0 || s == NULL) {
        return -1;
    }
    const unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *cp_out = c0;
        *used_out = 1;
        return 0;
    }
    if ((c0 & 0xE0) == 0xC0 && len >= 2) {
        *cp_out = ((uint32_t)(c0 & 0x1F) << 6) | (s[1] & 0x3F);
        *used_out = 2;
        return 0;
    }
    if ((c0 & 0xF0) == 0xE0 && len >= 3) {
        *cp_out = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *used_out = 3;
        return 0;
    }
    if ((c0 & 0xF8) == 0xF0 && len >= 4) {
        *cp_out = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
                  ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *used_out = 4;
        return 0;
    }
    return -1;
}

static uint16_t lookup_gbk(uint32_t cp)
{
    if (cp < 0x80) {
        return (uint16_t)cp;
    }
    size_t lo = 0;
    size_t hi = s_utf8_gbk_map_len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t k = s_utf8_gbk_map[mid].cp;
        if (k == cp) {
            return s_utf8_gbk_map[mid].gbk;
        }
        if (k < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static size_t append_gbk_char(uint8_t *dst, size_t cap, size_t pos, uint32_t cp)
{
    if (pos >= cap) {
        return pos;
    }
    if (cp < 0x80) {
        dst[pos++] = (uint8_t)cp;
        return pos;
    }
    uint16_t gbk = lookup_gbk(cp);
    if (gbk == 0) {
        ESP_LOGW(TAG, "no GBK for U+%" PRIX32, cp);
        if (pos + 1 < cap) {
            dst[pos++] = '?';
        }
        return pos;
    }
    if (pos + 2 <= cap) {
        dst[pos++] = (uint8_t)(gbk >> 8);
        dst[pos++] = (uint8_t)(gbk & 0xFF);
    }
    return pos;
}

static uint32_t normalize_print_cp(uint32_t cp)
{
    switch (cp) {
    case 0x2013:
    case 0x2014:
        return (uint32_t)'-';
    case 0x2018:
    case 0x2019:
        return (uint32_t)'\'';
    case 0x201C:
    case 0x201D:
        return (uint32_t)'"';
    case 0x00A0:
        return (uint32_t)' ';
    default:
        return cp;
    }
}

static bool is_ascii_word_char(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return true;
    }
    if (cp >= 'a' && cp <= 'z') {
        return true;
    }
    if (cp >= '0' && cp <= '9') {
        return true;
    }
    return cp == '-' || cp == '\'' || cp == '_';
}

static bool is_ascii_break_char(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '-';
}

static unsigned char_display_cols(uint32_t cp)
{
    if (cp < 0x80) {
        return 1;
    }
    return 2;
}

static size_t encode_utf8_range(const char *line, size_t begin, size_t end, uint8_t *gbk_buf, size_t gbk_cap)
{
    size_t pos = 0;
    size_t i = begin;
    while (i < end) {
        uint32_t cp = 0;
        size_t used = 0;
        if (utf8_decode_one(line + i, end - i, &cp, &used) != 0) {
            i++;
            continue;
        }
        cp = normalize_print_cp(cp);
        pos = append_gbk_char(gbk_buf, gbk_cap, pos, cp);
        i += used;
    }
    return pos;
}

static esp_err_t emit_wrapped_line(escpos_link_write_fn write_fn, void *ctx, const char *line, size_t line_len,
                                   uint8_t *gbk_buf, size_t gbk_cap)
{
    const unsigned max_cols = (unsigned)CONFIG_UART_ESC_POS_PRINT_COLS;
    size_t i = 0;
    while (i < line_len) {
        size_t chunk_start = i;
        size_t pos = 0;
        unsigned cols = 0;
        size_t last_break = chunk_start;

        while (i < line_len) {
            uint32_t cp = 0;
            size_t used = 0;
            if (utf8_decode_one(line + i, line_len - i, &cp, &used) != 0) {
                i++;
                continue;
            }
            cp = normalize_print_cp(cp);
            unsigned w = char_display_cols(cp);
            if (cols + w > max_cols && cols > 0) {
                if (last_break > chunk_start) {
                    size_t emit_end = last_break;
                    while (emit_end > chunk_start) {
                        uint32_t prev_cp = 0;
                        size_t prev_used = 0;
                        size_t prev_off = emit_end;
                        while (prev_off > chunk_start) {
                            prev_off--;
                            if (utf8_decode_one(line + prev_off, line_len - prev_off, &prev_cp, &prev_used) == 0 &&
                                prev_off + prev_used == emit_end) {
                                break;
                            }
                        }
                        if (prev_off <= chunk_start ||
                            !is_ascii_break_char(normalize_print_cp(prev_cp))) {
                            break;
                        }
                        emit_end = prev_off;
                    }
                    pos = encode_utf8_range(line, chunk_start, emit_end, gbk_buf, gbk_cap);
                    i = last_break;
                    while (i < line_len) {
                        uint32_t sp = 0;
                        size_t sp_used = 0;
                        if (utf8_decode_one(line + i, line_len - i, &sp, &sp_used) != 0 ||
                            !is_ascii_break_char(normalize_print_cp(sp))) {
                            break;
                        }
                        i += sp_used;
                    }
                    goto emit_chunk;
                }
                break;
            }
            pos = append_gbk_char(gbk_buf, gbk_cap, pos, cp);
            cols += w;
            i += used;
            if (is_ascii_break_char(cp) || (cp < 0x80 && !is_ascii_word_char(cp))) {
                last_break = i;
            }
        }

    emit_chunk:
        if (pos == 0 && i > chunk_start) {
            pos = encode_utf8_range(line, chunk_start, i, gbk_buf, gbk_cap);
        } else if (pos == 0 && i < line_len) {
            uint32_t cp = 0;
            size_t used = 0;
            if (utf8_decode_one(line + i, line_len - i, &cp, &used) == 0) {
                cp = normalize_print_cp(cp);
                pos = append_gbk_char(gbk_buf, gbk_cap, pos, cp);
                i += used;
            } else {
                i = chunk_start + 1;
            }
        }
        if (pos > 0) {
            gbk_buf[pos++] = '\r';
            gbk_buf[pos++] = '\n';
            esp_err_t err = write_fn(ctx, gbk_buf, pos);
            if (err != ESP_OK) {
                return err;
            }
        } else if (i == chunk_start) {
            i++;
        }
    }
    return ESP_OK;
}

esp_err_t escpos_print_utf8_strip(const char *utf8, escpos_link_write_fn write_fn, void *ctx)
{
    if (utf8 == NULL || write_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (utf8[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t k_init[] = {0x1B, 0x40, 0x1C, 0x26, 0x1B, 0x4D, 0x01, 0x1B, 0x61, 0x00};
    static const uint8_t k_feed[] = {0x1B, 0x64, 0x05};

    esp_err_t err = write_fn(ctx, k_init, sizeof(k_init));
    if (err != ESP_OK) {
        return err;
    }

    const size_t gbk_cap = 512;
    uint8_t *gbk_buf = (uint8_t *)heap_caps_malloc(gbk_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gbk_buf == NULL) {
        gbk_buf = (uint8_t *)malloc(gbk_cap);
    }
    if (gbk_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const char *p = utf8;
    const char *line_start = p;
    while (*p != '\0') {
        if (*p == '\n' || *p == '\r') {
            size_t line_len = (size_t)(p - line_start);
            if (line_len > 0) {
                err = emit_wrapped_line(write_fn, ctx, line_start, line_len, gbk_buf, gbk_cap);
                if (err != ESP_OK) {
                    break;
                }
            }
            while (*p == '\n' || *p == '\r') {
                p++;
            }
            line_start = p;
            continue;
        }
        p++;
    }
    if (err == ESP_OK && p > line_start) {
        err = emit_wrapped_line(write_fn, ctx, line_start, (size_t)(p - line_start), gbk_buf, gbk_cap);
    }
    heap_caps_free(gbk_buf);

    if (err != ESP_OK) {
        return err;
    }
    return write_fn(ctx, k_feed, sizeof(k_feed));
}

static esp_err_t uart_link_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return uart_escpos_printer_send_bytes(data, len);
}

static esp_err_t usb_link_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return usb_escpos_printer_send_bytes(data, len);
}

esp_err_t escpos_printer_print_utf8(const char *utf8)
{
    if (utf8 == NULL || utf8[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (uart_escpos_printer_ready()) {
        return escpos_print_utf8_strip(utf8, uart_link_write, NULL);
    }
    if (usb_escpos_printer_ready()) {
        return escpos_print_utf8_strip(utf8, usb_link_write, NULL);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t escpos_printer_print_jpeg_file(const char *filepath)
{
    if (filepath == NULL || filepath[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (uart_escpos_printer_ready()) {
        return uart_escpos_print_jpeg_file(filepath);
    }
    if (usb_escpos_printer_ready()) {
        return usb_escpos_print_jpeg_file(filepath);
    }
    return ESP_ERR_NOT_FOUND;
}
