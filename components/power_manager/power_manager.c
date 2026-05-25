/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "power_manager.h"

#include "sdkconfig.h"

#if CONFIG_POWER_MANAGER_ENABLE

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "bsp/esp-bsp.h"
#include "bsp/wt99p4c5_s1_board.h"
#include "bsp/touch.h"
#include "esp_lcd_touch.h"

static const char *TAG = "power_mgr";

static void power_manager_task(void *arg);
static void power_manager_touch_isr_cb(esp_lcd_touch_handle_t tp);

#define PM_TASK_STACK          (3072)
#define PM_TASK_PRIO           (2)
#define PM_CHECK_PERIOD_MS     (500)
#define PM_WAKE_NOTIFY_BIT     (1U << 0)

static TaskHandle_t s_pm_task;
static SemaphoreHandle_t s_block_mux;
static uint32_t s_block_mask;
static int s_saved_brightness = 100;
static volatile bool s_screen_on = true;
static volatile bool s_wake_pending;

__attribute__((weak)) void power_manager_on_screen_off(void)
{
}

__attribute__((weak)) void power_manager_on_screen_on(void)
{
}

esp_err_t power_manager_init(void)
{
    if (s_pm_task != NULL) {
        return ESP_OK;
    }

    s_block_mux = xSemaphoreCreateMutex();
    if (s_block_mux == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_touch_handle_t tp = bsp_touch_get_handle();
    if (tp != NULL) {
        esp_err_t err = esp_lcd_touch_register_interrupt_callback(tp, power_manager_touch_isr_cb);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "touch wake ISR: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "no touch handle; wake relies on GPIO poll only");
    }

    BaseType_t ok = xTaskCreate(power_manager_task, "power_mgr", PM_TASK_STACK, NULL, PM_TASK_PRIO, &s_pm_task);
    if (ok != pdPASS) {
        vSemaphoreDelete(s_block_mux);
        s_block_mux = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "init idle_timeout=%d ms", CONFIG_POWER_MANAGER_IDLE_TIMEOUT_MS);
    return ESP_OK;
}

void power_manager_set_backlight_percent(int brightness_percent)
{
    if (brightness_percent < 0) {
        brightness_percent = 0;
    } else if (brightness_percent > 100) {
        brightness_percent = 100;
    }
    if (brightness_percent > 0) {
        s_saved_brightness = brightness_percent;
    }
}

void power_manager_notify_activity(void)
{
    s_wake_pending = true;
    if (s_pm_task != NULL) {
        xTaskNotify(s_pm_task, PM_WAKE_NOTIFY_BIT, eSetBits);
    }
    if (!s_screen_on) {
        return;
    }
    if (bsp_display_lock(0)) {
        lv_disp_trig_activity(NULL);
        bsp_display_unlock();
    }
}

bool power_manager_is_screen_on(void)
{
    return s_screen_on;
}

void power_manager_block_sleep(const char *tag)
{
    (void)tag;
    if (s_block_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_block_mux, portMAX_DELAY);
    s_block_mask++;
    xSemaphoreGive(s_block_mux);
    power_manager_notify_activity();
}

void power_manager_unblock_sleep(const char *tag)
{
    (void)tag;
    if (s_block_mux == NULL) {
        return;
    }
    xSemaphoreTake(s_block_mux, portMAX_DELAY);
    if (s_block_mask > 0) {
        s_block_mask--;
    }
    xSemaphoreGive(s_block_mux);
}

static bool sleep_blocked(void)
{
    bool blocked = false;
    if (s_block_mux != NULL) {
        xSemaphoreTake(s_block_mux, portMAX_DELAY);
        blocked = (s_block_mask != 0);
        xSemaphoreGive(s_block_mux);
    }
    return blocked;
}

static void wifi_ps_for_screen(bool screen_on)
{
    wifi_ps_type_t ps = screen_on ? WIFI_PS_MIN_MODEM : WIFI_PS_MAX_MODEM;
    esp_err_t err = esp_wifi_set_ps(ps);
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGD(TAG, "esp_wifi_set_ps(%d): %s", (int)ps, esp_err_to_name(err));
    }
}

static bool touch_active_now(void)
{
    esp_lcd_touch_handle_t tp = bsp_touch_get_handle();
    if (tp == NULL) {
        return (gpio_get_level(BSP_LCD_TOUCH_INT) == 0);
    }

    esp_err_t err = esp_lcd_touch_read_data(tp);
    if (err != ESP_OK) {
        return (gpio_get_level(BSP_LCD_TOUCH_INT) == 0);
    }

    esp_lcd_touch_point_data_t pt[1] = {0};
    uint8_t cnt = 0;
    err = esp_lcd_touch_get_data(tp, pt, &cnt, 1);
    return (err == ESP_OK && cnt > 0);
}

static void enter_screen_off(void)
{
    if (!s_screen_on) {
        return;
    }

    ESP_LOGI(TAG, "screen off (idle %d ms)", CONFIG_POWER_MANAGER_IDLE_TIMEOUT_MS);

    if (bsp_display_lock(100)) {
        lvgl_port_stop();
        bsp_display_unlock();
    }

    bsp_display_brightness_set(0);
    wifi_ps_for_screen(false);
    power_manager_on_screen_off();

    s_screen_on = false;
    s_wake_pending = false;
}

static void exit_screen_off(void)
{
    if (s_screen_on) {
        return;
    }

    ESP_LOGI(TAG, "screen on");

    wifi_ps_for_screen(true);
    power_manager_on_screen_on();

    bsp_display_brightness_set(s_saved_brightness > 0 ? s_saved_brightness : 100);

    if (bsp_display_lock(100)) {
        lvgl_port_resume();
        lv_disp_trig_activity(NULL);
        bsp_display_unlock();
    }

    s_screen_on = true;
    s_wake_pending = false;
}

void IRAM_ATTR power_manager_touch_isr_cb(esp_lcd_touch_handle_t tp)
{
    (void)tp;
    s_wake_pending = true;
    BaseType_t yield = pdFALSE;
    if (s_pm_task != NULL) {
        xTaskNotifyFromISR(s_pm_task, PM_WAKE_NOTIFY_BIT, eSetBits, &yield);
    }
    if (yield) {
        portYIELD_FROM_ISR();
    }
}

void power_manager_task(void *arg)
{
    (void)arg;
    uint32_t notify;

    for (;;) {
        if (xTaskNotifyWait(0, PM_WAKE_NOTIFY_BIT, &notify, 0) == pdTRUE || s_wake_pending) {
            s_wake_pending = false;
            if (!s_screen_on) {
                exit_screen_off();
            } else if (bsp_display_lock(0)) {
                lv_disp_trig_activity(NULL);
                bsp_display_unlock();
            }
        }

        if (s_screen_on) {
            if (!sleep_blocked()) {
                uint32_t inactive_ms = 0;
                if (bsp_display_lock(0)) {
                    inactive_ms = lv_disp_get_inactive_time(NULL);
                    bsp_display_unlock();
                }
                if (inactive_ms >= (uint32_t)CONFIG_POWER_MANAGER_IDLE_TIMEOUT_MS) {
                    enter_screen_off();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(PM_CHECK_PERIOD_MS));
        } else {
            if (touch_active_now()) {
                exit_screen_off();
                power_manager_notify_activity();
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_POWER_MANAGER_TOUCH_POLL_MS));
        }
    }
}

#else /* !CONFIG_POWER_MANAGER_ENABLE */

esp_err_t power_manager_init(void)
{
    return ESP_OK;
}

void power_manager_set_backlight_percent(int brightness_percent)
{
    (void)brightness_percent;
}

void power_manager_notify_activity(void) {}

void power_manager_block_sleep(const char *tag)
{
    (void)tag;
}

void power_manager_unblock_sleep(const char *tag)
{
    (void)tag;
}

bool power_manager_is_screen_on(void)
{
    return true;
}

void power_manager_on_screen_off(void) {}
void power_manager_on_screen_on(void) {}

#endif /* CONFIG_POWER_MANAGER_ENABLE */
