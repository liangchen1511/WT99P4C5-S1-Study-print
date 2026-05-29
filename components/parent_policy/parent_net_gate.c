/*
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 */
#include "parent_net_gate.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "parent_rst";

bool parent_net_sta_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif == NULL || esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return false;
    }
    return ip.ip.addr != 0;
}

bool parent_net_http_allowed(void)
{
    return parent_net_sta_has_ip();
}

void parent_reset_nvs_boot_log(void)
{
    const esp_reset_reason_t cur = esp_reset_reason();
    uint8_t last = 255;
    nvs_handle_t h;
    if (nvs_open("parent_rst", NVS_READWRITE, &h) == ESP_OK) {
        (void)nvs_get_u8(h, "last_rst", &last);
        ESP_LOGI(TAG, "boot reset this=%d last=%u", (int)cur, (unsigned)last);
        (void)nvs_set_u8(h, "last_rst", (uint8_t)cur);
        (void)nvs_commit(h);
        nvs_close(h);
    }
}

void parent_reset_log_phase(const char *phase)
{
    ESP_LOGI(TAG, "phase=%s reset=%d", phase != NULL ? phase : "?", (int)esp_reset_reason());
}
