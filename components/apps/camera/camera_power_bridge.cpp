/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Strong overrides for power_manager weak hooks (camera + SDIO WiFi background traffic).
 */
#include "power_manager.h"
#include "Camera.hpp"
#include "parent_album_sync.h"
#include "parent_chat/parent_chat_api.hpp"
#include "parent_policy.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

static const char *TAG = "cam_pwr";

static esp_timer_handle_t s_resume_timer;
static esp_timer_handle_t s_stagger_chat;
static esp_timer_handle_t s_stagger_policy;
static bool s_guard_registered;

static volatile bool s_have_ip;
static volatile bool s_screen_allows;

static void pause_bg_http(const char *reason)
{
    parent_album_sync_pause(true);
    parent_chat_bg_pause(true);
    parent_policy_poll_pause(true);
    ESP_LOGI(TAG, "bg HTTP paused (%s)", reason != nullptr ? reason : "?");
}

static void stop_resume_timers(void)
{
    if (s_resume_timer != nullptr) {
        esp_timer_stop(s_resume_timer);
    }
    if (s_stagger_chat != nullptr) {
        esp_timer_stop(s_stagger_chat);
    }
    if (s_stagger_policy != nullptr) {
        esp_timer_stop(s_stagger_policy);
    }
}

static void resume_album_now(void)
{
    if (!s_have_ip || !s_screen_allows || !power_manager_is_screen_on()) {
        return;
    }
    parent_album_sync_pause(false);
    ESP_LOGI(TAG, "album sync resumed");
}

static void stagger_chat_cb(void *arg)
{
    (void)arg;
    if (!s_have_ip || !s_screen_allows || !power_manager_is_screen_on()) {
        return;
    }
    parent_chat_bg_pause(false);
    ESP_LOGI(TAG, "chat poll resumed");
}

static void stagger_policy_cb(void *arg)
{
    (void)arg;
    if (!s_have_ip || !s_screen_allows || !power_manager_is_screen_on()) {
        return;
    }
    parent_policy_poll_pause(false);
    ESP_LOGI(TAG, "policy poll resumed");
}

static void resume_staggered(const char *reason)
{
    if (!s_have_ip || !power_manager_is_screen_on()) {
        pause_bg_http("no ip or screen off");
        return;
    }
    s_screen_allows = true;
    ESP_LOGI(TAG, "bg HTTP stagger resume (%s)", reason != nullptr ? reason : "?");
    resume_album_now();
    if (s_stagger_chat != nullptr) {
        esp_timer_stop(s_stagger_chat);
        esp_timer_start_once(s_stagger_chat, 1500 * 1000);
    }
    if (s_stagger_policy != nullptr) {
        esp_timer_stop(s_stagger_policy);
        esp_timer_start_once(s_stagger_policy, 3000 * 1000);
    }
}

static void resume_timer_cb(void *arg)
{
    (void)arg;
    resume_staggered("delayed");
}

static void schedule_bg_resume(uint32_t delay_ms, const char *reason)
{
    stop_resume_timers();
    pause_bg_http(reason);
    if (s_resume_timer == nullptr || !s_have_ip || !power_manager_is_screen_on()) {
        return;
    }
    esp_timer_start_once(s_resume_timer, (uint64_t)delay_ms * 1000ULL);
}

static void on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    s_have_ip = true;
    schedule_bg_resume(5000, "got ip");
}

static void on_lost_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    (void)event_data;
    s_have_ip = false;
    stop_resume_timers();
    pause_bg_http("lost ip");
}

static void ensure_bg_guard(void)
{
    if (s_guard_registered) {
        return;
    }
    s_have_ip = false;
    s_screen_allows = true;

    const esp_timer_create_args_t resume_args = {
        .callback = &resume_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bg_resume",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&resume_args, &s_resume_timer) != ESP_OK) {
        ESP_LOGW(TAG, "resume timer create failed");
        return;
    }

    const esp_timer_create_args_t chat_args = {
        .callback = &stagger_chat_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bg_chat",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&chat_args, &s_stagger_chat) != ESP_OK) {
        ESP_LOGW(TAG, "chat timer create failed");
        return;
    }

    const esp_timer_create_args_t pol_args = {
        .callback = &stagger_policy_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bg_pol",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&pol_args, &s_stagger_policy) != ESP_OK) {
        ESP_LOGW(TAG, "policy timer create failed");
        return;
    }

    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, nullptr, nullptr);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &on_lost_ip, nullptr, nullptr);
    s_guard_registered = true;
    pause_bg_http("boot");
    ESP_LOGI(TAG, "SDIO bg guard ready");
}

extern "C" void camera_power_bridge_init(void)
{
    ensure_bg_guard();
}

extern "C" void power_manager_on_screen_off(void)
{
    ensure_bg_guard();
    Camera::stopPreviewStreamingIfRunning();
    s_screen_allows = false;
    stop_resume_timers();
    pause_bg_http("screen off");
}

extern "C" void power_manager_on_screen_on(void)
{
    ensure_bg_guard();
    (void)Camera::ensurePreviewStreaming();
    s_screen_allows = true;
    schedule_bg_resume(3000, "screen on");
}
