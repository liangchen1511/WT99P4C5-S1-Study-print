/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/stat.h>
#include "esp_check.h"
#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

#include "gui_music/lv_demo_music.h"
#include "gui_music/lv_demo_music_main.h"
#include "MusicPlayer.hpp"
#include "parent_guard.hpp"
#include "camera/Camera.hpp"
#include "audio_player.h"
#include "esp_heap_caps.h"

#define MUSIC_DIR BSP_SD_MOUNT_POINT

using namespace std;

LV_IMG_DECLARE(img_app_music_player);

static const char *TAG = "MusicPlayer";

MusicPlayer::MusicPlayer()
    : ESP_Brookesia_PhoneApp("音乐", &img_app_music_player, true), // auto_resize_visual_area
      _file_iterator(NULL)
{
}

MusicPlayer::~MusicPlayer()
{
    if (_file_iterator != NULL) {
        file_iterator_delete(_file_iterator);
        _file_iterator = NULL;
    }
}

bool MusicPlayer::scanAudioFiles(void)
{
    if (_file_iterator != NULL) {
        file_iterator_delete(_file_iterator);
        _file_iterator = NULL;
    }

    struct stat st;
    if (stat(MUSIC_DIR, &st) != 0) {
        ESP_LOGW(TAG, "SD not mounted or missing: %s", MUSIC_DIR);
        return false;
    }

    if (bsp_extra_audio_file_instance_init(MUSIC_DIR, &_file_iterator) != ESP_OK) {
        ESP_LOGW(TAG, "No .mp3/.wav on SD root (%s)", MUSIC_DIR);
        return false;
    }

    ESP_LOGI(TAG, "Found %u audio file(s) on SD", (unsigned)file_iterator_get_count(_file_iterator));
    return true;
}

void MusicPlayer::showNoMusicUi(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);

    lv_obj_t *lbl = lv_label_create(parent);
    char msg[256];
    snprintf(msg, sizeof(msg),
             "No music on SD root (%s).\n\n"
             "Put *.mp3 or *.wav on the SD card root.\n"
             "Insert SD card and reopen this app.",
             MUSIC_DIR);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, BSP_LCD_H_RES - 40);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

bool MusicPlayer::run(void)
{
    if (!parent_guard_app_run("music")) {
        return false;
    }

    ESP_LOGI(TAG, "run: heap=%u min=%u", (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    Camera::stopPreviewStreamingIfRunning();

    if (_file_iterator == NULL) {
        scanAudioFiles();
    }

    if (_file_iterator == NULL) {
        showNoMusicUi(lv_scr_act());
        return true;
    }

    lv_demo_music(lv_scr_act(), _file_iterator);

    return true;
}

bool MusicPlayer::pause(void)
{
    if (_file_iterator != NULL) {
        _lv_demo_music_pause();
    }
    audio_player_pause();

    return true;
}

bool MusicPlayer::back(void)
{
    notifyCoreClosed();

    return true;
}

bool MusicPlayer::close(void)
{
    if (_file_iterator != NULL) {
        lv_demo_music_close();
    }
    if (audio_player_stop() != ESP_OK) {
        ESP_LOGW(TAG, "audio_player_stop failed");
    }

    return true;
}

bool MusicPlayer::init(void)
{
    if (bsp_extra_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "Audio player init failed");
        return false;
    }

    scanAudioFiles();

    return true;
}
