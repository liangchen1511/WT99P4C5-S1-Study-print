/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "jpeg_decoder.h"

#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

#include "sdkconfig.h"

#include "usb_escpos_printer.h"
#include "escpos_jpeg_raster.h"

static const char *TAG = "usb_escpos";

#ifndef CONFIG_USB_ESC_POS_MAX_WIDTH
#define CONFIG_USB_ESC_POS_MAX_WIDTH 384
#endif

/** CDC ACM line rate (menuconfig USB_ESC_POS_CDC_BAUD; EM5820H default 9600). */
#ifndef CONFIG_USB_ESC_POS_CDC_BAUD
#define CONFIG_USB_ESC_POS_CDC_BAUD 9600
#endif

#ifndef USB_CLASS_CDC_DATA
#define USB_CLASS_CDC_DATA 0x0a
#endif

/* USB CDC PSTN subclass ACM class requests (communicated to Communication interface). */
#define USB_CDC_REQ_SET_LINE_CODING          0x20
#define USB_CDC_REQ_SET_CONTROL_LINE_STATE   0x22

#define USB_CHUNK_BYTES        512

static usb_host_client_handle_t s_client;
static usb_device_handle_t      s_dev;
static uint8_t                  s_intf;
static uint8_t                  s_bulk_ep;
static uint16_t                 s_ep_mps;
/** Communication interface for CDC class requests; 0xFF = not CDC / skip */
static uint8_t                  s_cdc_comm_intf = 0xFF;
static SemaphoreHandle_t        s_io_mux;
static SemaphoreHandle_t        s_tx_done;
static volatile bool            s_inited;

static void usb_client_task(void *arg);

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);

static bool config_find_bulk_out_for_class(const usb_config_desc_t *cfg, uint8_t iface_class,
                                           uint8_t *intf_out, uint8_t *alt_out, uint8_t *ep_out, uint16_t *mps_out);

static bool config_find_bulk_out_escpos(const usb_config_desc_t *cfg, uint8_t *intf_out, uint8_t *alt_out,
                                        uint8_t *ep_out, uint16_t *mps_out, uint8_t *cdc_comm_intf_out);

/** Last resort: any Bulk OUT (e.g. bInterfaceClass==0 “per interface” or buggy descriptors). Skips hub/HID/MSC. */
static bool config_find_any_bulk_out_escpos(const usb_config_desc_t *cfg, uint8_t *intf_out, uint8_t *alt_out,
                                            uint8_t *ep_out, uint16_t *mps_out, uint8_t *cdc_comm_intf_out);

static void log_usb_config_interfaces(const usb_config_desc_t *cfg);

static int8_t cdc_find_comm_interface(const usb_config_desc_t *cfg, uint8_t data_intf_num);

static esp_err_t control_xfer_out_no_data(usb_device_handle_t dev, uint8_t bmRequestType, uint8_t bRequest,
                                          uint16_t wValue, uint16_t wIndex);

static esp_err_t control_xfer_out_with_data(usb_device_handle_t dev, uint8_t bmRequestType, uint8_t bRequest,
                                            uint16_t wValue, uint16_t wIndex, const uint8_t *payload, uint16_t payload_len);

static esp_err_t cdc_acm_startup(usb_device_handle_t dev, uint8_t comm_intf);

static esp_err_t attach_printer(uint8_t dev_addr);

static void detach_printer(usb_device_handle_t gone_hdl);

static esp_err_t bulk_out_send(const uint8_t *data, size_t len);

static void xfer_done_cb(usb_transfer_t *xfer);

unsigned usb_escpos_printer_max_width(void)
{
    return escpos_jpeg_raster_max_width();
}

static esp_err_t usb_escpos_link_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    return bulk_out_send(data, len);
}

bool usb_escpos_printer_ready(void)
{
    return s_dev != NULL;
}

esp_err_t usb_escpos_printer_send_bytes(const uint8_t *data, size_t len)
{
    return bulk_out_send(data, len);
}

static bool config_find_bulk_out_for_class(const usb_config_desc_t *cfg, uint8_t iface_class,
                                           uint8_t *intf_out, uint8_t *alt_out, uint8_t *ep_out, uint16_t *mps_out)
{
    const uint16_t total_len = cfg->wTotalLength;

    for (uint8_t intf = 0; intf < 16; intf++) {
        const int num_alt = usb_parse_interface_number_of_alternate(cfg, intf);
        if (num_alt < 1) {
            continue;
        }
        for (uint8_t alt = 0; alt < (uint8_t)num_alt; alt++) {
            int intf_offset = 0;
            const usb_intf_desc_t *id = usb_parse_interface_descriptor(cfg, intf, alt, &intf_offset);
            if (id == NULL) {
                break;
            }
            if (id->bInterfaceClass != iface_class) {
                continue;
            }
            for (int ep_ix = 0; ep_ix < id->bNumEndpoints; ep_ix++) {
                int ep_offset = intf_offset;
                const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(id, ep_ix, total_len, &ep_offset);
                if (ep == NULL) {
                    continue;
                }
                if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (USB_EP_DESC_GET_EP_DIR(ep) != 0) { /* 0 = OUT */
                    continue;
                }
                *intf_out = intf;
                *alt_out = alt;
                *ep_out = ep->bEndpointAddress;
                *mps_out = USB_EP_DESC_GET_MPS(ep);
                return true;
            }
        }
    }
    return false;
}

static int8_t cdc_find_comm_interface(const usb_config_desc_t *cfg, uint8_t data_intf_num)
{
    if (data_intf_num > 0) {
        int off = 0;
        const usb_intf_desc_t *prev = usb_parse_interface_descriptor(cfg, (uint8_t)(data_intf_num - 1), 0, &off);
        if (prev != NULL && prev->bInterfaceClass == USB_CLASS_COMM) {
            return (int8_t)(data_intf_num - 1);
        }
    }
    for (uint8_t intf = 0; intf < 16; intf++) {
        const int num_alt = usb_parse_interface_number_of_alternate(cfg, intf);
        if (num_alt < 1) {
            continue;
        }
        int intf_offset = 0;
        const usb_intf_desc_t *id = usb_parse_interface_descriptor(cfg, intf, 0, &intf_offset);
        if (id != NULL && id->bInterfaceClass == USB_CLASS_COMM) {
            return (int8_t)intf;
        }
    }
    return -1;
}

/**
 * Prefer CDC Data (虚拟串口的数据 Bulk OUT)，再标准打印类，再 Vendor。
 */
static bool config_find_bulk_out_escpos(const usb_config_desc_t *cfg, uint8_t *intf_out, uint8_t *alt_out,
                                        uint8_t *ep_out, uint16_t *mps_out, uint8_t *cdc_comm_intf_out)
{
    *cdc_comm_intf_out = 0xFF;

    static const uint8_t k_order[] = {
        USB_CLASS_CDC_DATA,
        USB_CLASS_PRINTER,
        USB_CLASS_VENDOR_SPEC,
    };

    for (size_t i = 0; i < sizeof(k_order); i++) {
        if (!config_find_bulk_out_for_class(cfg, k_order[i], intf_out, alt_out, ep_out, mps_out)) {
            continue;
        }
        if (k_order[i] == USB_CLASS_CDC_DATA) {
            int8_t comm = cdc_find_comm_interface(cfg, *intf_out);
            *cdc_comm_intf_out = (comm >= 0) ? (uint8_t)comm : 0xFF;
            if (*cdc_comm_intf_out == 0xFF) {
                ESP_LOGW(TAG, "CDC Data intf=%u but no COMM intf found; skipping CDC line setup", *intf_out);
            }
        }
        return true;
    }

    if (config_find_any_bulk_out_escpos(cfg, intf_out, alt_out, ep_out, mps_out, cdc_comm_intf_out)) {
        ESP_LOGW(TAG, "Using bulk-OUT fallback (intf=%u); device may use non-standard interface class", *intf_out);
        return true;
    }
    return false;
}

static bool config_find_any_bulk_out_escpos(const usb_config_desc_t *cfg, uint8_t *intf_out, uint8_t *alt_out,
                                            uint8_t *ep_out, uint16_t *mps_out, uint8_t *cdc_comm_intf_out)
{
    *cdc_comm_intf_out = 0xFF;
    const uint16_t total_len = cfg->wTotalLength;

    for (uint8_t intf = 0; intf < 16; intf++) {
        const int num_alt = usb_parse_interface_number_of_alternate(cfg, intf);
        if (num_alt < 1) {
            continue;
        }
        for (uint8_t alt = 0; alt < (uint8_t)num_alt; alt++) {
            int intf_offset = 0;
            const usb_intf_desc_t *id = usb_parse_interface_descriptor(cfg, intf, alt, &intf_offset);
            if (id == NULL) {
                break;
            }
            const uint8_t cls = id->bInterfaceClass;
            if (cls == USB_CLASS_HUB || cls == USB_CLASS_HID || cls == USB_CLASS_MASS_STORAGE) {
                continue;
            }
            for (int ep_ix = 0; ep_ix < id->bNumEndpoints; ep_ix++) {
                int ep_offset = intf_offset;
                const usb_ep_desc_t *ep =
                    usb_parse_endpoint_descriptor_by_index(id, ep_ix, total_len, &ep_offset);
                if (ep == NULL) {
                    continue;
                }
                if (USB_EP_DESC_GET_XFERTYPE(ep) != USB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if (USB_EP_DESC_GET_EP_DIR(ep) != 0) {
                    continue;
                }
                *intf_out = intf;
                *alt_out = alt;
                *ep_out = ep->bEndpointAddress;
                *mps_out = USB_EP_DESC_GET_MPS(ep);
                if (cls == USB_CLASS_CDC_DATA || cls == USB_CLASS_PER_INTERFACE) {
                    int8_t comm = cdc_find_comm_interface(cfg, intf);
                    *cdc_comm_intf_out = (comm >= 0) ? (uint8_t)comm : 0xFF;
                }
                return true;
            }
        }
    }
    return false;
}

static void log_usb_config_interfaces(const usb_config_desc_t *cfg)
{
    const uint16_t total_len = cfg->wTotalLength;
    for (uint8_t intf = 0; intf < 16; intf++) {
        const int num_alt = usb_parse_interface_number_of_alternate(cfg, intf);
        if (num_alt < 1) {
            continue;
        }
        int intf_offset = 0;
        const usb_intf_desc_t *id = usb_parse_interface_descriptor(cfg, intf, 0, &intf_offset);
        if (id == NULL) {
            continue;
        }
        ESP_LOGI(TAG, "cfg intf[%u] class=%u subclass=%u eps=%u alts=%d", intf, id->bInterfaceClass,
                 id->bInterfaceSubClass, id->bNumEndpoints, num_alt);
        for (int ep_ix = 0; ep_ix < id->bNumEndpoints; ep_ix++) {
            int ep_off = intf_offset;
            const usb_ep_desc_t *ep =
                usb_parse_endpoint_descriptor_by_index(id, ep_ix, total_len, &ep_off);
            if (ep == NULL) {
                continue;
            }
            ESP_LOGI(TAG, "  ep 0x%02x xfertype=%u mps=%u dir_in=%d", ep->bEndpointAddress,
                     (unsigned)USB_EP_DESC_GET_XFERTYPE(ep), (unsigned)USB_EP_DESC_GET_MPS(ep),
                     (int)USB_EP_DESC_GET_EP_DIR(ep));
        }
    }
}

static esp_err_t control_xfer_out_no_data(usb_device_handle_t dev, uint8_t bmRequestType, uint8_t bRequest,
                                          uint16_t wValue, uint16_t wIndex)
{
    usb_setup_packet_t setup = {};
    setup.bmRequestType = bmRequestType;
    setup.bRequest = bRequest;
    setup.wValue = wValue;
    setup.wIndex = wIndex;
    setup.wLength = 0;

    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(8, 0, &xfer);
    if (err != ESP_OK || xfer == NULL) {
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }
    memcpy(xfer->data_buffer, &setup.val[0], 8);

    xfer->device_handle = dev;
    xfer->callback = xfer_done_cb;
    xfer->context = (void *)s_tx_done;
    xfer->num_bytes = 8;

    xSemaphoreTake(s_tx_done, 0);
    err = usb_host_transfer_submit_control(s_client, xfer);
    if (err != ESP_OK) {
        usb_host_transfer_free(xfer);
        return err;
    }
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        usb_host_transfer_free(xfer);
        return ESP_ERR_TIMEOUT;
    }
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        usb_host_transfer_free(xfer);
        return ESP_FAIL;
    }
    usb_host_transfer_free(xfer);
    return ESP_OK;
}

static esp_err_t control_xfer_out_with_data(usb_device_handle_t dev, uint8_t bmRequestType, uint8_t bRequest,
                                            uint16_t wValue, uint16_t wIndex, const uint8_t *payload,
                                            uint16_t payload_len)
{
    usb_setup_packet_t setup = {};
    setup.bmRequestType = bmRequestType;
    setup.bRequest = bRequest;
    setup.wValue = wValue;
    setup.wIndex = wIndex;
    setup.wLength = payload_len;

    const size_t total = 8 + payload_len;
    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(total, 0, &xfer);
    if (err != ESP_OK || xfer == NULL) {
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }
    memcpy(xfer->data_buffer, &setup.val[0], 8);
    if (payload_len != 0 && payload != NULL) {
        memcpy(xfer->data_buffer + 8, payload, payload_len);
    }

    xfer->device_handle = dev;
    xfer->callback = xfer_done_cb;
    xfer->context = (void *)s_tx_done;
    xfer->num_bytes = (int)total;

    xSemaphoreTake(s_tx_done, 0);
    err = usb_host_transfer_submit_control(s_client, xfer);
    if (err != ESP_OK) {
        usb_host_transfer_free(xfer);
        return err;
    }
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
        usb_host_transfer_free(xfer);
        return ESP_ERR_TIMEOUT;
    }
    if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        usb_host_transfer_free(xfer);
        return ESP_FAIL;
    }
    usb_host_transfer_free(xfer);
    return ESP_OK;
}

static esp_err_t cdc_acm_startup(usb_device_handle_t dev, uint8_t comm_intf)
{
    const uint8_t typ_class_if_out =
        USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;

    /* Many模组在未拉高 DTR 时不接收主机下发的 TX 数据 */
    esp_err_t err =
        control_xfer_out_no_data(dev, typ_class_if_out, USB_CDC_REQ_SET_CONTROL_LINE_STATE,
                                 /* DTR=1, RTS=1 */ 3, comm_intf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SET_CONTROL_LINE_STATE: %s", esp_err_to_name(err));
    }

    const uint32_t baud = (uint32_t)CONFIG_USB_ESC_POS_CDC_BAUD;
    uint8_t lc[7] = {
        (uint8_t)(baud & 0xff),
        (uint8_t)((baud >> 8) & 0xff),
        (uint8_t)((baud >> 16) & 0xff),
        (uint8_t)((baud >> 24) & 0xff),
        0, /* 1 stop bit */
        0, /* no parity */
        8,
    };

    err = control_xfer_out_with_data(dev, typ_class_if_out, USB_CDC_REQ_SET_LINE_CODING, 0, comm_intf, lc,
                                     sizeof(lc));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SET_LINE_CODING @ %" PRIu32 " baud: %s", baud, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "CDC ACM: line coding %" PRIu32 " 8N1, DTR/RTS set on intf %u", baud, comm_intf);
    }
    return ESP_OK;
}

static esp_err_t attach_printer(uint8_t dev_addr)
{
    if (s_dev != NULL) {
        ESP_LOGW(TAG, "Printer already open; ignoring new device addr=%u", dev_addr);
        return ESP_OK;
    }

    usb_device_handle_t dev_hdl = NULL;
    esp_err_t err = usb_host_device_open(s_client, dev_addr, &dev_hdl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device_open: %s", esp_err_to_name(err));
        return err;
    }

    const usb_config_desc_t *cfg = NULL;
    err = usb_host_get_active_config_descriptor(dev_hdl, &cfg);
    if (err != ESP_OK || cfg == NULL) {
        ESP_LOGE(TAG, "get_active_config_desc failed");
        usb_host_device_close(s_client, dev_hdl);
        return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    }

    uint8_t intf = 0, alt = 0, ep = 0;
    uint16_t mps = 64;
    uint8_t cdc_comm = 0xFF;
    if (!config_find_bulk_out_escpos(cfg, &intf, &alt, &ep, &mps, &cdc_comm)) {
        ESP_LOGW(TAG, "No usable bulk-OUT on addr=%u (dump interfaces below)", dev_addr);
        log_usb_config_interfaces(cfg);
        usb_host_device_close(s_client, dev_hdl);
        return ESP_ERR_NOT_FOUND;
    }

    err = usb_host_interface_claim(s_client, dev_hdl, intf, alt);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "interface_claim %u alt %u failed: %s", intf, alt, esp_err_to_name(err));
        usb_host_device_close(s_client, dev_hdl);
        return err;
    }

    s_dev = dev_hdl;
    s_intf = intf;
    s_bulk_ep = ep;
    s_ep_mps = mps;
    s_cdc_comm_intf = cdc_comm;

    if (cdc_comm != 0xFF) {
        (void)cdc_acm_startup(dev_hdl, cdc_comm);
    }

    ESP_LOGI(TAG, "ESC/POS link ready: intf=%u ep=0x%02x mps=%u%s", intf, ep, (unsigned)mps,
             cdc_comm != 0xFF ? " (CDC ACM)" : "");
    return ESP_OK;
}

static void detach_printer(usb_device_handle_t gone_hdl)
{
    if (gone_hdl == NULL || gone_hdl != s_dev) {
        return;
    }
    esp_err_t err = usb_host_interface_release(s_client, s_dev, s_intf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "interface_release: %s", esp_err_to_name(err));
    }
    err = usb_host_device_close(s_client, s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device_close: %s", esp_err_to_name(err));
    }
    s_dev = NULL;
    s_bulk_ep = 0;
    s_ep_mps = 0;
    s_cdc_comm_intf = 0xFF;
    ESP_LOGI(TAG, "Printer disconnected");
}

static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)arg;
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        esp_err_t err = attach_printer(event_msg->new_dev.address);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "attach_printer: %s", esp_err_to_name(err));
        }
    } else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        detach_printer(event_msg->dev_gone.dev_hdl);
    }
}

static void xfer_done_cb(usb_transfer_t *xfer)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)xfer->context;
    if (sem) {
        xSemaphoreGive(sem);
    }
}

static esp_err_t bulk_out_send(const uint8_t *data, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_io_mux, portMAX_DELAY);

    usb_transfer_t *xfer = NULL;
    esp_err_t err = usb_host_transfer_alloc(USB_CHUNK_BYTES, 0, &xfer);
    if (err != ESP_OK || xfer == NULL) {
        xSemaphoreGive(s_io_mux);
        return err != ESP_OK ? err : ESP_ERR_NO_MEM;
    }

    xfer->callback = xfer_done_cb;
    xfer->context = (void *)s_tx_done;
    xfer->device_handle = s_dev;
    xfer->bEndpointAddress = s_bulk_ep;

    for (size_t off = 0; off < len; off += USB_CHUNK_BYTES) {
        size_t chunk = len - off;
        if (chunk > USB_CHUNK_BYTES) {
            chunk = USB_CHUNK_BYTES;
        }
        memcpy(xfer->data_buffer, data + off, chunk);
        xfer->num_bytes = (int)chunk;

        xSemaphoreTake(s_tx_done, 0);

        err = usb_host_transfer_submit(xfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "transfer_submit failed: %s", esp_err_to_name(err));
            break;
        }
        if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(8000)) != pdTRUE) {
            ESP_LOGE(TAG, "transfer timeout");
            err = ESP_ERR_TIMEOUT;
            break;
        }
        if (xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
            ESP_LOGE(TAG, "transfer status=%d actual=%d req=%d", (int)xfer->status, xfer->actual_num_bytes,
                     xfer->num_bytes);
            err = ESP_FAIL;
            break;
        }
    }

    usb_host_transfer_free(xfer);
    xSemaphoreGive(s_io_mux);
    return err;
}

static void usb_client_task(void *arg)
{
    (void)arg;
    /*
     * If the printer was already plugged when the host stack enumerated it, NEW_DEV may have
     * been emitted before usb_host_client_register() — ESP-IDF does not replay it to new clients.
     * Scan connected addresses after a short settle time so CDC/Printer devices are not missed.
     */
    vTaskDelay(pdMS_TO_TICKS(150));
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill((int)sizeof(addrs), addrs, &n) == ESP_OK && n > 0) {
        ESP_LOGI(TAG, "USB: %d device(s) already connected; probing for ESC/POS", n);
        for (int i = 0; i < n && s_dev == NULL; i++) {
            esp_err_t aerr = attach_printer(addrs[i]);
            if (aerr != ESP_OK && aerr != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "attach_printer addr=%u: %s", (unsigned)addrs[i], esp_err_to_name(aerr));
            }
        }
    }

    while (1) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

esp_err_t usb_escpos_printer_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_io_mux = xSemaphoreCreateMutex();
    s_tx_done = xSemaphoreCreateBinary();
    if (s_io_mux == NULL || s_tx_done == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };

    esp_err_t err = usb_host_client_register(&cfg, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client_register: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreate(usb_client_task, "usb_printer", 4096, NULL, 6, NULL);
    if (ok != pdTRUE) {
        usb_host_client_deregister(s_client);
        s_client = NULL;
        return ESP_FAIL;
    }

    s_inited = true;
    ESP_LOGI(TAG, "USB ESC/POS client registered (waiting for device)");
    return ESP_OK;
}

esp_err_t usb_escpos_print_jpeg_file(const char *filepath)
{
    if (filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dev == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return escpos_jpeg_raster_print(filepath, usb_escpos_link_write, NULL);
}
