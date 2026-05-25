/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE ESC/POS: NimBLE GAP central + Nordic UART Service (same byte stream as Dongwei UART send_init).
 * Board: ESP32-P4 + ESP-Hosted ESP32-C5 (BLE only, VHCI) — see esp-hosted examples/host_nimble_bleprph_host_only_vhci.
 */

#include "sdkconfig.h"

#if defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE

#ifndef CONFIG_BLE_ESC_POS_NAME_FILTER
#define CONFIG_BLE_ESC_POS_NAME_FILTER ""
#endif

#include <inttypes.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "store/config/ble_store_config.h"

void ble_store_config_init(void);

#include "ble_escpos_printer.h"
#include "escpos_jpeg_raster.h"

static const char *TAG = "ble_escpos";

#define NVS_NS_BLE_PRN "ble_prn"
#define NVS_KEY_ADDR  "addr"

static const ble_uuid128_t g_nus_svc =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t g_nus_rx =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_nus_rx_handle;
static uint16_t s_nus_sh;
static uint16_t s_nus_eh;
static uint16_t s_att_mtu = 23;
static bool s_scanning;
static bool s_connecting;
static SemaphoreHandle_t s_wr_sem;
static int s_wr_status;
static SemaphoreHandle_t s_link_mux;

static uint8_t s_saved_peer[6];
static bool s_have_saved_peer;
static ble_addr_t s_pending_peer;
static bool s_have_pending_peer;

static void load_saved_peer(void);
static esp_err_t save_peer_nvs(const uint8_t addr[6]);

static int gap_event(struct ble_gap_event *event, void *arg);
static void host_task(void *param);
static void on_reset(int reason);
static void on_sync(void);
static void start_scan_if_needed(void);

static esp_err_t save_peer_nvs(const uint8_t addr[6])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS_BLE_PRN, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_ADDR, addr, 6);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        memcpy(s_saved_peer, addr, 6);
        s_have_saved_peer = true;
    }
    return err;
}

static void load_saved_peer(void)
{
    s_have_saved_peer = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_BLE_PRN, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_saved_peer);
    if (nvs_get_blob(h, NVS_KEY_ADDR, s_saved_peer, &len) == ESP_OK && len == 6) {
        s_have_saved_peer = true;
        ESP_LOGI(TAG, "Saved printer addr %02x:%02x:%02x:%02x:%02x:%02x", s_saved_peer[5], s_saved_peer[4],
                 s_saved_peer[3], s_saved_peer[2], s_saved_peer[1], s_saved_peer[0]);
    }
    nvs_close(h);
}

static bool addr_matches_saved(const struct ble_gap_disc_desc *d)
{
    if (!s_have_saved_peer) {
        return false;
    }
    return memcmp(d->addr.val, s_saved_peer, 6) == 0;
}

static bool adv_has_nus_or_name_ok(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) {
        return false;
    }

    if (CONFIG_BLE_ESC_POS_NAME_FILTER[0] != '\0') {
        if (fields.name == NULL || fields.name_len == 0) {
            return false;
        }
        char name[32];
        size_t n = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
        memcpy(name, fields.name, n);
        name[n] = '\0';
        if (strcasestr(name, CONFIG_BLE_ESC_POS_NAME_FILTER) != NULL) {
            ESP_LOGI(TAG, "Name match: %s", name);
            return true;
        }
        return false;
    }

    for (unsigned i = 0; i < fields.num_uuids128; i++) {
        if (ble_uuid_cmp(&fields.uuids128[i].u, &g_nus_svc.u) == 0) {
            return true;
        }
    }
    return false;
}

static bool should_connect(const struct ble_gap_disc_desc *d)
{
    if (addr_matches_saved(d)) {
        return true;
    }
    return adv_has_nus_or_name_ok(d);
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                         void *arg);

static int disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc,
                       void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (s_nus_sh != 0 && s_nus_eh != 0) {
            int rc = ble_gattc_disc_all_chrs(conn_handle, s_nus_sh, s_nus_eh, disc_chr_cb, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "disc chrs rc=%d", rc);
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            ESP_LOGW(TAG, "NUS service not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        s_nus_sh = 0;
        s_nus_eh = 0;
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGW(TAG, "disc svc err=%d", error->status);
        return 0;
    }
    if (svc == NULL) {
        return 0;
    }
    if (ble_uuid_cmp(&svc->uuid.u, &g_nus_svc.u) == 0) {
        s_nus_sh = svc->start_handle;
        s_nus_eh = svc->end_handle;
        ESP_LOGI(TAG, "NUS service %" PRIu16 "-%" PRIu16, s_nus_sh, s_nus_eh);
    }
    return 0;
}

static int disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr,
                         void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (s_nus_rx_handle == 0) {
            ESP_LOGW(TAG, "NUS RX not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            ESP_LOGI(TAG, "BLE ESC/POS ready (rx_handle=%u mtu=%u)", (unsigned)s_nus_rx_handle, (unsigned)s_att_mtu);
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGW(TAG, "disc chr err=%d", error->status);
        return 0;
    }
    if (chr == NULL) {
        return 0;
    }
    if (ble_uuid_cmp(&chr->uuid.u, &g_nus_rx.u) == 0) {
        s_nus_rx_handle = chr->val_handle;
        ESP_LOGI(TAG, "NUS RX val_handle=%u", (unsigned)s_nus_rx_handle);
    }
    return 0;
}

static int mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t mtu, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (error != NULL && error->status == 0 && mtu >= 23) {
        s_att_mtu = mtu;
    }
    return 0;
}

static int gatt_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr,
                         void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    s_wr_status = (error == NULL) ? BLE_HS_EBADDATA : error->status;
    if (s_wr_sem) {
        xSemaphoreGive(s_wr_sem);
    }
    return 0;
}

static esp_err_t ble_link_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_nus_rx_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_wr_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const unsigned chunk = (s_att_mtu > 3) ? (unsigned)(s_att_mtu - 3) : 20;
    xSemaphoreTake(s_link_mux, portMAX_DELAY);

    esp_err_t out = ESP_OK;
    for (size_t off = 0; off < len && out == ESP_OK; off += chunk) {
        size_t n = len - off;
        if (n > chunk) {
            n = chunk;
        }
        xSemaphoreTake(s_wr_sem, 0);
        int rc = ble_gattc_write_flat(s_conn_handle, s_nus_rx_handle, data + off, n, gatt_write_cb, NULL);
        if (rc != 0) {
            out = ESP_FAIL;
            break;
        }
        if (xSemaphoreTake(s_wr_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
            out = ESP_ERR_TIMEOUT;
            break;
        }
        if (s_wr_status != 0) {
            ESP_LOGW(TAG, "GATT write status=%d", s_wr_status);
            out = ESP_FAIL;
            break;
        }
    }

    xSemaphoreGive(s_link_mux);
    return out;
}

bool ble_escpos_printer_ready(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE && s_nus_rx_handle != 0;
}

esp_err_t ble_escpos_print_jpeg_file(const char *filepath)
{
    if (!ble_escpos_printer_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return escpos_jpeg_raster_print(filepath, ble_link_write, NULL);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_params cp = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,
        .itvl_max = BLE_GAP_INITIAL_CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = 0x0100,
        .min_ce_len = 0x0000,
        .max_ce_len = 0x0000,
    };

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_connecting || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            return 0;
        }
        const struct ble_gap_disc_desc *d = &event->disc;
        if (!should_connect(d)) {
            return 0;
        }
        (void)ble_gap_disc_cancel();
        s_scanning = false;

        memcpy(&s_pending_peer, &d->addr, sizeof(s_pending_peer));
        s_have_pending_peer = true;

        ESP_LOGI(TAG, "Connecting…");
        s_connecting = true;
        int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &d->addr, BLE_HS_FOREVER, &cp, gap_event, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gap_connect failed rc=%d", rc);
            s_connecting = false;
            s_have_pending_peer = false;
            start_scan_if_needed();
        }
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_nus_rx_handle = 0;
            s_have_pending_peer = false;
            start_scan_if_needed();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        ESP_LOGI(TAG, "connected handle=%u", (unsigned)s_conn_handle);
        if (s_have_pending_peer) {
            (void)save_peer_nvs(s_pending_peer.val);
            s_have_pending_peer = false;
        }
        s_nus_rx_handle = 0;
        s_nus_sh = 0;
        s_nus_eh = 0;
        s_att_mtu = 23;
        ble_gattc_exchange_mtu(s_conn_handle, mtu_cb, NULL);
        ble_gattc_disc_all_svcs(s_conn_handle, disc_svc_cb, NULL);
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_nus_rx_handle = 0;
        s_att_mtu = 23;
        start_scan_if_needed();
        return 0;

    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_conn_handle && event->mtu.value > s_att_mtu) {
            s_att_mtu = event->mtu.value;
            ESP_LOGI(TAG, "ATT MTU=%u", (unsigned)s_att_mtu);
        }
        return 0;

    default:
        return 0;
    }
}

static void start_scan_if_needed(void)
{
    if (s_scanning || s_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_connecting) {
        return;
    }
    struct ble_gap_disc_params dp = {.filter_duplicates = 1};
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    } else {
        s_scanning = true;
        ESP_LOGI(TAG, "Scanning for BLE UART (NUS) printer…");
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr rc=%d", rc);
        return;
    }
    load_saved_peer();
    start_scan_if_needed();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_escpos_printer_init(void)
{
    static bool s_done;

    if (s_done) {
        return ESP_OK;
    }

    s_wr_sem = xSemaphoreCreateBinary();
    s_link_mux = xSemaphoreCreateMutex();
    if (s_wr_sem == NULL || s_link_mux == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_hosted_connect_to_slave();

    esp_err_t err = esp_hosted_bt_controller_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hosted_bt_controller_init: %s", esp_err_to_name(err));
    }
    err = esp_hosted_bt_controller_enable();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_hosted_bt_controller_enable: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_mitm = 0;

    ble_store_config_init();

    int rc = ble_svc_gap_device_name_set("WT99P4C5");
    if (rc != 0) {
        ESP_LOGW(TAG, "device_name_set rc=%d", rc);
    }

    nimble_port_freertos_init(host_task);
    s_done = true;
    ESP_LOGI(TAG, "BLE ESC/POS printer stack started");
    return ESP_OK;
}

#else /* !(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) */

#include "ble_escpos_printer.h"
#include "esp_err.h"

esp_err_t ble_escpos_printer_init(void)
{
    return ESP_OK;
}

bool ble_escpos_printer_ready(void)
{
    return false;
}

esp_err_t ble_escpos_print_jpeg_file(const char *filepath)
{
    (void)filepath;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE */
