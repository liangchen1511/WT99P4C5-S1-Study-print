/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "esp_brookesia.hpp"
#include "sdkconfig.h"
#include "apps.h"
#include "boot_splash.h"
#include "parent_policy.hpp"
#include "parent_album_sync.h"
#include "camera/camera_power_bridge.h"
#include "power_manager.h"
#include "escpos_feature_flags.h"
#include "uart_escpos_printer.h"
#include "usb_escpos_printer.h"
#define LVGL_PORT_INIT_CONFIG()   \
    {                             \
        .task_priority = 4,       \
        .task_stack = 10 * 1024,  \
        .task_affinity = -1,      \
        .task_max_sleep_ms = 500, \
        .timer_period_ms = 5,     \
    }

static const char *TAG = "app_main";

static SemaphoreHandle_t s_deferred_init_done;

#if CONFIG_UART_ESC_POS_BOOT_TEST
static void uart_printer_boot_test_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5000));
    if (!uart_escpos_printer_ready()) {
        ESP_LOGW(TAG, "UART printer not ready, skip boot test");
        vTaskDelete(nullptr);
        return;
    }
    esp_err_t e1 = uart_escpos_printer_test_hello();
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_err_t e2 = uart_escpos_printer_test_small_123();
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_err_t e3 = uart_escpos_printer_test_demo_format();
    ESP_LOGI(TAG, "UART boot tests: hello=%s 123=%s demo=%s", esp_err_to_name(e1), esp_err_to_name(e2),
             esp_err_to_name(e3));
    vTaskDelete(nullptr);
}
#endif

static void deferred_init_task(void *arg)
{
    (void)arg;

    esp_err_t err = bsp_spiffs_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SPIFFS mount successfully");
    }

    err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed (%s), continue without SD card", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SD card mount successfully");
    }

    err = bsp_extra_codec_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Codec init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Codec init successfully");
    }

    err = bsp_eth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Ethernet init successfully");
    }

    {
        esp_err_t uh = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false);
        if (uh != ESP_OK) {
            ESP_LOGW(TAG, "USB Host start failed: %s", esp_err_to_name(uh));
        } else {
            uh = usb_escpos_printer_init();
            if (uh != ESP_OK) {
                ESP_LOGW(TAG, "USB ESC/POS printer init failed: %s", esp_err_to_name(uh));
            }
        }
    }

    {
        esp_err_t uarts = uart_escpos_printer_init();
        if (uarts != ESP_OK) {
            ESP_LOGW(TAG, "UART ESC/POS init failed: %s", esp_err_to_name(uarts));
        }
#if CONFIG_UART_ESC_POS_BOOT_TEST
        else if (uart_escpos_printer_ready()) {
            xTaskCreate(uart_printer_boot_test_task, "uart_prn_test", 4096, nullptr, 5, nullptr);
        }
#endif
    }

    xSemaphoreGive(s_deferred_init_done);
    vTaskDelete(nullptr);
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .hw_cfg =
            {
                .hdmi_resolution = BSP_HDMI_RES_NONE,
                .dsi_bus =
                    {
                        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                        .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
                    },
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = true,
                .sw_rotate = false,
            },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    (void)disp;
    bsp_display_backlight_on();

    s_deferred_init_done = xSemaphoreCreateBinary();
    assert(s_deferred_init_done != nullptr);
    xTaskCreate(deferred_init_task, "def_init", 8192, nullptr, 5, nullptr);

    ESP_LOGI(TAG, "Boot splash");
    bsp_display_lock(0);

    boot_splash_show(lv_scr_act());
    boot_splash_run();
    boot_splash_dismiss();

    if (xSemaphoreTake(s_deferred_init_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "Deferred init still running after splash");
    }
    vSemaphoreDelete(s_deferred_init_done);
    s_deferred_init_done = nullptr;

    ESP_LOGI(TAG, "Display ESP-Brookesia phone demo");

    ESP_Brookesia_Phone *phone = new ESP_Brookesia_Phone();
    assert(phone != nullptr && "Failed to create phone");

    ESP_Brookesia_PhoneStylesheet_t *phone_stylesheet =
        new ESP_Brookesia_PhoneStylesheet_t ESP_BROOKESIA_PHONE_1024_600_DARK_STYLESHEET();
    ESP_BROOKESIA_CHECK_NULL_EXIT(phone_stylesheet, "Create phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->addStylesheet(*phone_stylesheet), "Add phone stylesheet failed");
    ESP_BROOKESIA_CHECK_FALSE_EXIT(phone->activateStylesheet(*phone_stylesheet), "Activate phone stylesheet failed");

    assert(phone->begin() && "Failed to begin phone");

    parent_policy_init();
    parent_album_sync_bg_start();
    camera_power_bridge_init();

    ESP_ERROR_CHECK(power_manager_init());
    power_manager_set_backlight_percent(100);

    Calculator *calculator = new Calculator();
    assert(calculator != nullptr && "Failed to create calculator");
    assert((phone->installApp(calculator) >= 0) && "Failed to begin calculator");
    MusicPlayer *music_player = new MusicPlayer();
    assert(music_player != nullptr && "Failed to create music_player");
    assert((phone->installApp(music_player) >= 0) && "Failed to begin music_player");

    AppSettings *app_settings = new AppSettings();
    assert(app_settings != nullptr && "Failed to create app_settings");
    assert((phone->installApp(app_settings) >= 0) && "Failed to begin app_settings");

    Game2048 *game_2048 = new Game2048();
    assert(game_2048 != nullptr && "Failed to create game_2048");
    assert((phone->installApp(game_2048) >= 0) && "Failed to begin game_2048");

    Camera *camera = new Camera(1280, 960);
    assert(camera != nullptr && "Failed to create camera");
    assert((phone->installApp(camera) >= 0) && "Failed to begin camera");

    AppVideoPlayer *video_player = new AppVideoPlayer();
    assert(video_player != nullptr && "Failed to create video_player");
    assert((phone->installApp(video_player) >= 0) && "Failed to begin video_player");

    SoTi *so_ti = new SoTi(1280, 960);
    assert(so_ti != nullptr && "Failed to create SoTi");
    int so_ti_app_id = phone->installApp(*so_ti);
    assert(so_ti_app_id >= 0 && "Failed to begin SoTi");

    PhotoAlbum *photo_album = new PhotoAlbum(so_ti_app_id);
    assert(photo_album != nullptr && "Failed to create photo_album");
    assert((phone->installApp(*photo_album) >= 0) && "Failed to begin photo_album");

    Print *print_app = new Print();
    assert(print_app != nullptr && "Failed to create print");
    assert((phone->installApp(print_app) >= 0) && "Failed to begin print");

    ParentChat *parent_chat = new ParentChat();
    assert(parent_chat != nullptr && "Failed to create parent_chat");
    assert((phone->installApp(parent_chat) >= 0) && "Failed to begin parent_chat");

    bsp_display_unlock();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
