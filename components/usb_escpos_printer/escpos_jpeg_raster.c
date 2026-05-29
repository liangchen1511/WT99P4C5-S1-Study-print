/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_err.h"
#include "jpeg_decoder.h"

#include "escpos_jpeg_raster.h"
#include "uart_escpos_printer.h"

static const char *TAG = "escpos_raster";

#ifndef CONFIG_USB_ESC_POS_MAX_WIDTH
#define CONFIG_USB_ESC_POS_MAX_WIDTH 384
#endif

#ifndef CONFIG_ESC_POS_RASTER_BAND_HEIGHT
#define CONFIG_ESC_POS_RASTER_BAND_HEIGHT 24
#endif
#ifndef CONFIG_ESC_POS_RASTER_BAND_DELAY_MS
#define CONFIG_ESC_POS_RASTER_BAND_DELAY_MS 100
#endif
#ifndef CONFIG_ESC_POS_RASTER_FINISH_FEED_DOTS
#define CONFIG_ESC_POS_RASTER_FINISH_FEED_DOTS 32
#endif
#ifndef CONFIG_UART_ESC_POS_FLOW_CTS_GPIO
#define CONFIG_UART_ESC_POS_FLOW_CTS_GPIO -1
#endif

#define JPEG_CAP_BYTES         (4 * 1024 * 1024)
#define JPEG_OUT_MAX_PIXELS    (1600 * 1600)

unsigned escpos_jpeg_raster_max_width(void)
{
    return CONFIG_USB_ESC_POS_MAX_WIDTH;
}

static uint8_t rgb565_to_gray(uint16_t p)
{
    unsigned r = (p >> 11) & 0x1f;
    unsigned g = (p >> 5) & 0x3f;
    unsigned b = p & 0x1f;
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (uint8_t)((r * 77 + g * 151 + b * 28) >> 8);
}

static esp_err_t escpos_raster_finish_safe(escpos_link_write_fn write_fn, void *ctx)
{
    uint8_t fin[3] = {0x1B, 0x4A, (uint8_t)CONFIG_ESC_POS_RASTER_FINISH_FEED_DOTS};
    esp_err_t err = write_fn(ctx, fin, sizeof(fin));
    if (err != ESP_OK) {
        return err;
    }
    static const uint8_t reset[] = {0x1B, 0x40};
    return write_fn(ctx, reset, sizeof(reset));
}

static esp_err_t escpos_pack_band(const uint8_t *mono, unsigned width, unsigned y0, unsigned band_h,
                                  unsigned threshold, uint8_t *packed)
{
    const unsigned width_bytes = (width + 7) / 8;
    for (unsigned y = 0; y < band_h; y++) {
        const unsigned src_y = y0 + y;
        for (unsigned xb = 0; xb < width_bytes; xb++) {
            uint8_t byte = 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                unsigned x = xb * 8 + bit;
                bool black = (x < width) && (mono[src_y * width + x] < threshold);
                if (black) {
                    byte |= (uint8_t)(1u << (7 - bit));
                }
            }
            packed[y * width_bytes + xb] = byte;
        }
    }
    return ESP_OK;
}

static esp_err_t escpos_raster_gs_v0_banded(escpos_link_write_fn write_fn, void *ctx, const uint8_t *mono,
                                            unsigned width, unsigned height)
{
    const unsigned width_bytes = (width + 7) / 8;
    const unsigned threshold = 140;
    const unsigned band_max = (unsigned)CONFIG_ESC_POS_RASTER_BAND_HEIGHT;
    if (band_max < 1) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t init_printer[] = {0x1B, 0x40};
    esp_err_t err = write_fn(ctx, init_printer, sizeof(init_printer));
    if (err != ESP_OK) {
        return err;
    }

    const size_t band_cap = (size_t)width_bytes * (size_t)(band_max + 8);
    uint8_t *packed = (uint8_t *)heap_caps_malloc(band_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (packed == NULL) {
        packed = (uint8_t *)malloc(band_cap);
    }
    if (packed == NULL) {
        return ESP_ERR_NO_MEM;
    }

    unsigned y0 = 0;
    unsigned bands = 0;
    while (y0 < height && err == ESP_OK) {
        unsigned bh = height - y0;
        if (bh > band_max) {
            bh = band_max;
        }
        unsigned send_h = bh;
#if CONFIG_ESC_POS_RASTER_PAD_48
        if (send_h > 0 && (send_h % 48) == 0 && y0 + bh >= height) {
            send_h = bh + 8;
        }
#endif
        memset(packed, 0xFF, (size_t)width_bytes * send_h);
        err = escpos_pack_band(mono, width, y0, bh, threshold, packed);
        if (err != ESP_OK) {
            break;
        }

        uint8_t hdr[8] = {0x1D, 0x76, 0x30, 0x00,
                          (uint8_t)(width_bytes & 0xff), (uint8_t)((width_bytes >> 8) & 0xff),
                          (uint8_t)(send_h & 0xff), (uint8_t)((send_h >> 8) & 0xff)};
        err = write_fn(ctx, hdr, sizeof(hdr));
        if (err == ESP_OK) {
            err = write_fn(ctx, packed, (size_t)width_bytes * send_h);
        }
        bands++;
        y0 += bh;
        if ((bands % 8) == 0) {
            const int cts = uart_escpos_cts_gpio_level();
            ESP_LOGI(TAG, "band %u y=%u/%u cts_gpio%d=%d", bands, y0, height,
                     CONFIG_UART_ESC_POS_FLOW_CTS_GPIO, cts);
        }
        if (y0 < height && err == ESP_OK && CONFIG_ESC_POS_RASTER_BAND_DELAY_MS > 0) {
            const int cts0 = uart_escpos_cts_gpio_level();
            vTaskDelay(pdMS_TO_TICKS(CONFIG_ESC_POS_RASTER_BAND_DELAY_MS));
            const int cts1 = uart_escpos_cts_gpio_level();
            if (cts0 >= 0 || cts1 >= 0) {
                ESP_LOGI(TAG, "band %u delay %dms cts %d->%d", bands, CONFIG_ESC_POS_RASTER_BAND_DELAY_MS, cts0, cts1);
            }
        }
    }
    heap_caps_free(packed);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "banded_print total_h=%u band_h=%u bands=%u", height, band_max, bands);
    return escpos_raster_finish_safe(write_fn, ctx);
}

static esp_err_t decode_jpeg_scaled_rgb565(const char *filepath, uint16_t **rgb565_out, unsigned *w_out, unsigned *h_out)
{
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long fsz = ftell(f);
    if (fsz <= 0 || (uint32_t)fsz > JPEG_CAP_BYTES) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);
    uint8_t *jpeg_buf = (uint8_t *)heap_caps_malloc((size_t)fsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (jpeg_buf == NULL) {
        jpeg_buf = (uint8_t *)malloc((size_t)fsz);
    }
    if (jpeg_buf == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(jpeg_buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        heap_caps_free(jpeg_buf);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);

    esp_jpeg_image_cfg_t jcfg = {};
    jcfg.indata = jpeg_buf;
    jcfg.indata_size = (uint32_t)fsz;
    jcfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    jcfg.flags.swap_color_bytes = 0;

    esp_jpeg_image_output_t jinfo = {};
    esp_err_t err = esp_jpeg_get_image_info(&jcfg, &jinfo);
    if (err != ESP_OK) {
        heap_caps_free(jpeg_buf);
        return err;
    }

    uint64_t px = (uint64_t)jinfo.width * (uint64_t)jinfo.height;
    if (px > JPEG_OUT_MAX_PIXELS) {
        heap_caps_free(jpeg_buf);
        return ESP_ERR_INVALID_SIZE;
    }

    const unsigned max_w = escpos_jpeg_raster_max_width();
    esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0,
        JPEG_IMAGE_SCALE_1_2,
        JPEG_IMAGE_SCALE_1_4,
        JPEG_IMAGE_SCALE_1_8,
    };

    uint16_t *out16 = NULL;
    size_t out_len = 0;

    for (size_t si = 0; si < sizeof(scales) / sizeof(scales[0]); si++) {
        jcfg.out_scale = scales[si];
        esp_jpeg_image_output_t jout_meta = {};
        err = esp_jpeg_get_image_info(&jcfg, &jout_meta);
        if (err != ESP_OK) {
            continue;
        }
        if (jout_meta.width > max_w && scales[si] != JPEG_IMAGE_SCALE_1_8) {
            continue;
        }

        out_len = jout_meta.output_len;
        out16 = (uint16_t *)heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (out16 == NULL) {
            out16 = (uint16_t *)malloc(out_len);
        }
        if (out16 == NULL) {
            heap_caps_free(jpeg_buf);
            return ESP_ERR_NO_MEM;
        }
        jcfg.outbuf = (uint8_t *)out16;
        jcfg.outbuf_size = (uint32_t)out_len;

        esp_jpeg_image_output_t img = {};
        err = esp_jpeg_decode(&jcfg, &img);
        if (err == ESP_OK) {
            *rgb565_out = out16;
            *w_out = img.width;
            *h_out = img.height;
            heap_caps_free(jpeg_buf);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "jpeg decode scale=%u failed: %s", (unsigned)si, esp_err_to_name(err));
        heap_caps_free(out16);
        out16 = NULL;
    }

    heap_caps_free(jpeg_buf);
    return ESP_FAIL;
}

static void downsample_box_gray(const uint16_t *src, unsigned sw, unsigned sh, uint8_t *dst, unsigned dw, unsigned dh)
{
    for (unsigned y = 0; y < dh; y++) {
        const unsigned y0 = y * sh / dh;
        for (unsigned x = 0; x < dw; x++) {
            const unsigned x0 = x * sw / dw;
            dst[y * dw + x] = rgb565_to_gray(src[y0 * sw + x0]);
        }
    }
}

esp_err_t escpos_jpeg_raster_print(const char *filepath, escpos_link_write_fn write_fn, void *ctx)
{
    if (filepath == NULL || write_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "raster_print start %s mode=GSv0-banded cts_gpio%d=%d", filepath,
             CONFIG_UART_ESC_POS_FLOW_CTS_GPIO, uart_escpos_cts_gpio_level());

    uint16_t *rgb565 = NULL;
    unsigned img_w = 0, img_h = 0;
    esp_err_t err = decode_jpeg_scaled_rgb565(filepath, &rgb565, &img_w, &img_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return err;
    }

    const unsigned max_w = escpos_jpeg_raster_max_width();
    unsigned dw = img_w;
    unsigned dh = img_h;
    if (dw > max_w) {
        dh = (unsigned)((uint64_t)dh * (uint64_t)max_w / (uint64_t)dw);
        dw = max_w;
        if (dh < 1) {
            dh = 1;
        }
    }
    const unsigned max_h = 900;
    if (dh > max_h) {
        dw = (unsigned)((uint64_t)dw * (uint64_t)max_h / (uint64_t)dh);
        dh = max_h;
        if (dw < 1) {
            dw = 1;
        }
    }

    uint8_t *gray = (uint8_t *)heap_caps_malloc((size_t)dw * dh, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (gray == NULL) {
        gray = (uint8_t *)malloc((size_t)dw * dh);
    }
    if (gray == NULL) {
        heap_caps_free(rgb565);
        return ESP_ERR_NO_MEM;
    }

    downsample_box_gray(rgb565, img_w, img_h, gray, dw, dh);
    heap_caps_free(rgb565);

    err = escpos_raster_gs_v0_banded(write_fn, ctx, gray, dw, dh);
    heap_caps_free(gray);
    ESP_LOGI(TAG, "raster_print done %s -> %" PRIu32 "x%" PRIu32 " err=%s", filepath, (uint32_t)dw, (uint32_t)dh,
             esp_err_to_name(err));
    return err;
}
