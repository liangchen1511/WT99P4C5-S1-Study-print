/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <algorithm>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"
#include "esp_lvgl_simple_player/media_src_storage.h"
#include "esp_lvgl_simple_player/esp_lvgl_simple_player.h"
#include "VideoPlayer.hpp"
#include "parent_guard.hpp"
#include "parent_policy.hpp"
#include "parent_chat/parent_chat_api.hpp"
#include "camera/Camera.hpp"
#include "audio_player.h"

#define APP_MAX_VIDEO_NUM           (15)
/* Compressed MJPEG frame input (not RGB565 framebuffer); keep small for DMA headroom */
#define APP_VIDEO_JPEG_IN_BUF_SIZE  (512 * 1024)
#define APP_CACHE_BUF_SIZE          (64 * 1024)
#define APP_VIDEO_DROPDOWN_H        (80)
#define APP_VIDEO_CONTROLS_H        (85)
#define APP_VIDEO_VIEWPORT_H        (BSP_LCD_V_RES - APP_VIDEO_DROPDOWN_H - APP_VIDEO_CONTROLS_H)

using namespace std;

static const char *TAG = "AppVideoPlayer";

static bool filename_is_mjpeg(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (ext == NULL) {
        return false;
    }
    return strcasecmp(ext, ".mjpeg") == 0;
}

LV_IMG_DECLARE(img_app_video_player);

typedef struct {
    char path[64];
} video_switch_msg_t;

static void video_switch_worker(void *arg)
{
    video_switch_msg_t *msg = (video_switch_msg_t *)arg;
    if (msg == NULL) {
        vTaskDelete(NULL);
        return;
    }
    esp_lvgl_simple_player_stop();
    esp_lvgl_simple_player_wait_task_stop(-1);
    esp_lvgl_simple_player_change_file(msg->path);
    esp_lvgl_simple_player_play();
    heap_caps_free(msg);
    vTaskDelete(NULL);
}

AppVideoPlayer::AppVideoPlayer():
    ESP_Brookesia_PhoneApp("视频", &img_app_video_player, true), // auto_resize_visual_area
    _video_name(NULL)
{
}

AppVideoPlayer::~AppVideoPlayer()
{
}

bool AppVideoPlayer::run(void)
{
    if (!parent_guard_app_run("video")) {
        return false;
    }

    ESP_LOGI(TAG, "run: heap=%u dma_int=%u psram=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    /* Free CSI DMA/PSRAM + JPEG encoder before HW MJPEG decode (rxlink needs INTERNAL DMA) */
    Camera::setFrameProcessingEnabled(false);
    Camera::releaseJpegEncoderHw();
    if (!Camera::releasePreviewPsramBuffers()) {
        ESP_LOGE(TAG, "camera stream did not release; refuse to race MJPEG decoder");
        (void)Camera::ensureJpegEncoderForHw();
        Camera::setFrameProcessingEnabled(true);
        return false;
    }
    parent_chat_bg_poll_stop();
    parent_chat_bg_poll_wait_stop(2000);
    parent_policy_poll_pause(true);
    audio_player_stop();
    esp_lvgl_simple_player_pause_background();

    app_show_ui();

    return true;
}

bool AppVideoPlayer::pause(void)
{
    /* Brookesia multitasking: fully stop the MJPEG task so recents/snapshot won't race LVGL. */
    esp_lvgl_simple_player_pause_background();

    return true;
}

bool AppVideoPlayer::resume(void)
{
    /* Continue after pause_background (STOPPED) or in-app pause (PAUSED). */
    esp_lvgl_simple_player_play();

    return true;
}

bool AppVideoPlayer::back(void)
{
    return notifyCoreClosed();
}

bool AppVideoPlayer::close(void)
{
    bsp_display_unlock();
    esp_lvgl_simple_player_del();
    bsp_display_lock(100);
    parent_policy_poll_pause(false);
    parent_chat_bg_poll_start();
    Camera::setFrameProcessingEnabled(true);
    if (!Camera::preallocateCaptureBuffers()) {
        ESP_LOGW(TAG, "camera buffers could not be reserved after video close");
    }

    return true;
}

bool AppVideoPlayer::init(void)
{
    return true;
}

void AppVideoPlayer::app_show_ui(void)
{
    uint8_t i = 0;
    int sel_file = searchMideaFiles();

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    /* Rows: vertical scroll so tall video + controls stay reachable after playback */
    lv_obj_t *cont_col = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cont_col, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_clear_flag(cont_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(cont_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont_col, 0, 0);
    lv_obj_set_flex_align(cont_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(cont_col, lv_color_black(), 0);
    lv_obj_set_style_border_width(cont_col, 0, 0);

    if (_midea_info_vect.empty()) {
        lv_obj_t *lbl = lv_label_create(cont_col);
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "No motion-JPEG on SD root (%s).\n\n"
                 "Only *.mjpeg is supported (JPEG frames in one file).\n"
                 "MP4 / H.264 will not play.",
                 BSP_SD_MOUNT_POINT);
        lv_label_set_text(lbl, msg);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, BSP_LCD_H_RES - 40);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_t *cont_row = lv_obj_create(cont_col);
    lv_obj_set_size(cont_row, BSP_LCD_H_RES - 20, 80);
    lv_obj_set_flex_flow(cont_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_top(cont_row, 2, 0);
    lv_obj_set_style_pad_bottom(cont_row, 2, 0);
    lv_obj_set_flex_align(cont_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(cont_row, lv_color_black(), 0);
    lv_obj_set_style_border_width(cont_row, 0, 0);

    /* Dropdown files */
    lv_obj_t * dd = lv_dropdown_create(cont_row);
    lv_dropdown_clear_options(dd);
    lv_obj_set_width(dd, BSP_LCD_H_RES / 3);
    for (auto &it : _midea_info_vect) {
        lv_dropdown_add_option(dd, it.video_name.c_str(), i);
        i++;
    }
    lv_obj_add_event_cb(dd, file_changed, LV_EVENT_VALUE_CHANGED, this);
    lv_dropdown_set_selected(dd, sel_file);
    lv_obj_set_style_pad_top(dd, 5, 0);

    /* Create player */
    snprintf(_video_path, sizeof(_video_path), "%s/%s", BSP_SD_MOUNT_POINT, _video_name);
    esp_lvgl_simple_player_cfg_t player_cfg = {
        .video_path = _video_path,
        .screen = cont_col,
        .buff_size = APP_VIDEO_JPEG_IN_BUF_SIZE,
        .cache_buff_size = APP_CACHE_BUF_SIZE,
        .cache_buff_in_psram = true,
        .screen_width = BSP_LCD_H_RES,
        .screen_height = APP_VIDEO_VIEWPORT_H + APP_VIDEO_CONTROLS_H,
        .viewport_width = BSP_LCD_H_RES,
        .viewport_height = APP_VIDEO_VIEWPORT_H,
        .flags = {
            .auto_width = false,
            .auto_height = false,
            .fit_letterbox = true,
        },
    };
    esp_lvgl_simple_player_create(&player_cfg);

    /* Start playing */
    esp_lvgl_simple_player_play();
}

uint8_t AppVideoPlayer::searchMideaFiles(void)
{
    int sel_file = 0;
    int i = 0;
    struct dirent *dir;
    DIR *d;

    _midea_info_vect.clear();

    // Search and store video files
    if (DIR *d = opendir(BSP_SD_MOUNT_POINT)) {
        while (struct dirent *dir = readdir(d)) {
            if (dir->d_type != DT_DIR && filename_is_mjpeg(dir->d_name)) {
                if (_midea_info_vect.size() >= APP_MAX_VIDEO_NUM) {
                    ESP_LOGE(TAG, "Too many video files");
                    break;
                }
                ESP_LOGI(TAG, "Found video file: %s", dir->d_name);
                _midea_info_vect.push_back({string(dir->d_name)});
            }
        }
        closedir(d);  // Always close the directory
    }

    // Select the video file based on 'sel_file'
    if (sel_file >= 0 && sel_file < _midea_info_vect.size()) {
        _video_name = _midea_info_vect[sel_file].video_name.c_str();
    }

    return sel_file;
}

void AppVideoPlayer::file_changed(lv_event_t * e)
{
    AppVideoPlayer *app = (AppVideoPlayer *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target(e);
    char *video_path = app->_video_path;
    char video_name[64];
    bool found_video_file = false;

    if(code == LV_EVENT_VALUE_CHANGED) {
        lv_dropdown_get_selected_str(obj, video_name, sizeof(video_name));
        snprintf(video_path, sizeof(app->_video_path), "%s/%s", BSP_SD_MOUNT_POINT, video_name);
        ESP_LOGI(TAG, "Selected file: %s", video_path);

        for (auto &it : app->_midea_info_vect) {
            if (strcmp(it.video_name.c_str(), video_name) == 0) {
                found_video_file = true;
                break;
            }
        }
        if (!found_video_file) {
            ESP_LOGE(TAG, "File not found in the map");
            return;
        }

        /* Run off the LVGL task: unlock/wait/lock inside dropdown callbacks corrupts the
         * recursive LVGL mutex (same issue as stop_event_cb if nested differently). */
        video_switch_msg_t *msg = (video_switch_msg_t *)heap_caps_malloc(sizeof(video_switch_msg_t), MALLOC_CAP_INTERNAL);
        if (msg == NULL) {
            ESP_LOGE(TAG, "video switch malloc failed");
            return;
        }
        strncpy(msg->path, video_path, sizeof(msg->path) - 1);
        msg->path[sizeof(msg->path) - 1] = '\0';
        if (xTaskCreate(video_switch_worker, "vid_sw", 8192, msg, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "video switch task create failed");
            heap_caps_free(msg);
        }
    }
}
