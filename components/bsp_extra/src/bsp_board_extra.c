/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "bsp/esp-bsp.h"
#include "bsp_board_extra.h"

static const char *TAG = "bsp_extra_board";

static esp_codec_dev_handle_t play_dev_handle;
static esp_codec_dev_handle_t record_dev_handle;

static bool _is_audio_init = false;
static bool _is_player_init = false;
static int _vloume_intensity = CODEC_DEFAULT_VOLUME;

static audio_player_cb_t audio_idle_callback = NULL;
static void *audio_idle_cb_user_data = NULL;
static char audio_file_path[128];

#define SHUTTER_SOUND_TASK_STACK    (3072)
#define SHUTTER_SOUND_TASK_PRIO     (5)

static void bsp_extra_shutter_sound_task(void *arg)
{
    (void)arg;

    if (!_is_audio_init || play_dev_handle == NULL) {
        vTaskDelete(NULL);
        return;
    }

    const uint32_t sr = CODEC_DEFAULT_SAMPLE_RATE;
    const unsigned dur_ms = 95;
    const size_t frames = (size_t)sr * dur_ms / 1000;
    const size_t n_int16 = frames * 2;

    int16_t *pcm = (int16_t *)malloc(n_int16 * sizeof(int16_t));
    if (pcm == NULL) {
        vTaskDelete(NULL);
        return;
    }

    uint32_t seed = (uint32_t)esp_timer_get_time();
    const float pi = 3.14159265358979323846f;
    for (size_t i = 0; i < frames; i++) {
        float t = (float)i / (float)sr;
        float env = expf(-t * 42.f);
        seed = seed * 1664525u + 1013904223u;
        float u = (float)(seed & 0xffffu) / 65536.f * 2.f - 1.f;
        float tone = sinf(2.f * pi * 2600.f * t) * expf(-t * 140.f);
        float mix = 0.52f * u + 0.48f * tone;
        int32_t s = (int32_t)(mix * 28000.f * env);
        if (s > 32767) {
            s = 32767;
        }
        if (s < -32768) {
            s = -32768;
        }
        pcm[2 * i] = (int16_t)s;
        pcm[2 * i + 1] = (int16_t)s;
    }

    if (bsp_extra_codec_set_fs(sr, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL) != ESP_OK) {
        free(pcm);
        vTaskDelete(NULL);
        return;
    }

    const size_t chunk_frames = 256;
    for (size_t f = 0; f < frames; f += chunk_frames) {
        size_t nf = frames - f;
        if (nf > chunk_frames) {
            nf = chunk_frames;
        }
        size_t nbytes = nf * 2 * sizeof(int16_t);
        size_t written = 0;
        (void)bsp_extra_i2s_write(&pcm[f * 2], nbytes, &written, pdMS_TO_TICKS(500));
    }

    free(pcm);
    vTaskDelete(NULL);
}

esp_err_t bsp_extra_play_camera_shutter_async(void)
{
    if (!_is_audio_init || play_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskCreate(bsp_extra_shutter_sound_task, "shutter_snd", SHUTTER_SOUND_TASK_STACK, NULL,
                    SHUTTER_SOUND_TASK_PRIO, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**************************************************************************************************
 *
 * Extra Board Function
 *
 **************************************************************************************************/

static esp_err_t audio_mute_function(AUDIO_PLAYER_MUTE_SETTING setting)
{
    // Volume saved when muting and restored when unmuting. Restoring volume is necessary
    // as es8311_set_voice_mute(true) results in voice volume (REG32) being set to zero.

    bsp_extra_codec_mute_set(setting == AUDIO_PLAYER_MUTE ? true : false);

    // restore the voice volume upon unmuting
    if (setting == AUDIO_PLAYER_UNMUTE) {
        ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, _vloume_intensity), TAG,
                            "Set Codec volume failed");
    }

    return ESP_OK;
}

static void audio_callback(audio_player_cb_ctx_t *ctx)
{
    if (audio_idle_callback) {
        ctx->user_ctx = audio_idle_cb_user_data;
        audio_idle_callback(ctx);
    }
}

esp_err_t bsp_extra_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_read(record_dev_handle, audio_buffer, len);
    *bytes_read = len;
    return ret;
}

esp_err_t bsp_extra_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_write(play_dev_handle, audio_buffer, len);
    *bytes_written = len;
    return ret;
}

esp_err_t bsp_extra_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch)
{
    esp_err_t ret = ESP_OK;

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = rate,
        .channel = ch,
        .bits_per_sample = bits_cfg,
    };

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_close(record_dev_handle);
        ret |= esp_codec_dev_set_in_gain(record_dev_handle, CODEC_DEFAULT_ADC_VOLUME);
    }

    if (play_dev_handle) {
        ret |= esp_codec_dev_open(play_dev_handle, &fs);
    }
    if (record_dev_handle) {
        ret |= esp_codec_dev_open(record_dev_handle, &fs);
    }
    return ret;
}

esp_err_t bsp_extra_codec_volume_set(int volume, int *volume_set)
{
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(play_dev_handle, volume), TAG, "Set Codec volume failed");
    _vloume_intensity = volume;

    ESP_LOGI(TAG, "Setting volume: %d", volume);

    return ESP_OK;
}

int bsp_extra_codec_volume_get(void)
{
    return _vloume_intensity;
}

esp_err_t bsp_extra_codec_mute_set(bool enable)
{
    esp_err_t ret = ESP_OK;
    ret = esp_codec_dev_set_out_mute(play_dev_handle, enable);
    return ret;
}

esp_err_t bsp_extra_codec_dev_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (play_dev_handle) {
        ret = esp_codec_dev_close(play_dev_handle);
    }

    if (record_dev_handle) {
        ret = esp_codec_dev_close(record_dev_handle);
    }
    return ret;
}

esp_err_t bsp_extra_codec_dev_resume(void)
{
    return bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);
}

esp_err_t bsp_extra_codec_init()
{
    if (_is_audio_init) {
        return ESP_OK;
    }

    play_dev_handle = bsp_audio_codec_speaker_init();
    assert((play_dev_handle) && "play_dev_handle not initialized");

    record_dev_handle = bsp_audio_codec_microphone_init();
    assert((record_dev_handle) && "record_dev_handle not initialized");

    bsp_extra_codec_set_fs(CODEC_DEFAULT_SAMPLE_RATE, CODEC_DEFAULT_BIT_WIDTH, CODEC_DEFAULT_CHANNEL);

    _is_audio_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_init(void)
{
    if (_is_player_init) {
        return ESP_OK;
    }

    audio_player_config_t config = {
        .mute_fn = audio_mute_function,
        .write_fn = bsp_extra_i2s_write,
        .clk_set_fn = bsp_extra_codec_set_fs,
        .priority = 5,
    };
    ESP_RETURN_ON_ERROR(audio_player_new(config), TAG, "audio_player_init failed");
    audio_player_callback_register(audio_callback, NULL);

    _is_player_init = true;

    return ESP_OK;
}

esp_err_t bsp_extra_player_del(void)
{
    _is_player_init = false;

    ESP_RETURN_ON_ERROR(audio_player_delete(), TAG, "audio_player_delete failed");

    return ESP_OK;
}

void file_iterator_delete(file_iterator_instance_t *i)
{
    if (i == NULL) {
        return;
    }
    if (i->list != NULL) {
        for (size_t j = 0; j < i->count; j++) {
            free(i->list[j]);
        }
        free(i->list);
    }
    if (i->directory_path != NULL) {
        free((void *)i->directory_path);
    }
    free(i);
}

esp_err_t bsp_extra_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    file_iterator_instance_t *file_iterator = file_iterator_new(path);
    ESP_RETURN_ON_FALSE(file_iterator, ESP_FAIL, TAG, "file_iterator_new failed, %s", path);

    *ret_instance = file_iterator;

    return ESP_OK;
}

static bool bsp_extra_is_audio_filename(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (ext == NULL) {
        return false;
    }
    return (strcasecmp(ext, ".mp3") == 0) || (strcasecmp(ext, ".wav") == 0);
}

static bool bsp_extra_dirent_is_audio(const struct dirent *ent)
{
    if (ent == NULL || ent->d_name[0] == '\0') {
        return false;
    }
    if (ent->d_name[0] == '.') {
        return false;
    }
    if (ent->d_type == DT_DIR) {
        return false;
    }
    return bsp_extra_is_audio_filename(ent->d_name);
}

esp_err_t bsp_extra_audio_file_instance_init(const char *path, file_iterator_instance_t **ret_instance)
{
    ESP_RETURN_ON_FALSE(path, ESP_FAIL, TAG, "path is NULL");
    ESP_RETURN_ON_FALSE(ret_instance, ESP_FAIL, TAG, "ret_instance is NULL");

    DIR *dir = opendir(path);
    ESP_RETURN_ON_FALSE(dir, ESP_FAIL, TAG, "opendir failed: %s", path);

    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (bsp_extra_dirent_is_audio(ent)) {
            count++;
        }
    }
    closedir(dir);

    if (count == 0) {
        ESP_LOGW(TAG, "No .mp3/.wav in %s", path);
        return ESP_FAIL;
    }

    file_iterator_instance_t *iter = calloc(1, sizeof(file_iterator_instance_t));
    ESP_RETURN_ON_FALSE(iter, ESP_FAIL, TAG, "calloc iterator failed");

    iter->count = count;
    iter->index = 0;
    iter->list = calloc(count, sizeof(char *));
    if (iter->list == NULL) {
        free(iter);
        return ESP_FAIL;
    }

    iter->directory_path = strdup(path);
    if (iter->directory_path == NULL) {
        free(iter->list);
        free(iter);
        return ESP_FAIL;
    }

    dir = opendir(path);
    if (dir == NULL) {
        free((void *)iter->directory_path);
        free(iter->list);
        free(iter);
        return ESP_FAIL;
    }

    size_t idx = 0;
    while ((ent = readdir(dir)) != NULL && idx < count) {
        if (!bsp_extra_dirent_is_audio(ent)) {
            continue;
        }
        iter->list[idx] = strdup(ent->d_name);
        if (iter->list[idx] == NULL) {
            closedir(dir);
            for (size_t j = 0; j < idx; j++) {
                free(iter->list[j]);
            }
            free(iter->list);
            free((void *)iter->directory_path);
            free(iter);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Audio file: %s", iter->list[idx]);
        idx++;
    }
    closedir(dir);

    *ret_instance = iter;
    return ESP_OK;
}

esp_err_t bsp_extra_player_play_index(file_iterator_instance_t *instance, int index)
{
    ESP_RETURN_ON_FALSE(instance, ESP_FAIL, TAG, "instance is NULL");

    ESP_LOGI(TAG, "play_index(%d)", index);
    char filename[128];
    int retval = file_iterator_get_full_path_from_index(instance, index, filename, sizeof(filename));
    ESP_RETURN_ON_FALSE(retval != 0, ESP_FAIL, TAG, "file_iterator_get_full_path_from_index failed");

    ESP_LOGI(TAG, "opening file '%s'", filename);
    FILE *fp = fopen(filename, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", filename);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, filename, sizeof(audio_file_path));

    return ESP_OK;
}

esp_err_t bsp_extra_player_play_file(const char *file_path)
{
    ESP_LOGI(TAG, "opening file '%s'", file_path);
    FILE *fp = fopen(file_path, "rb");
    ESP_RETURN_ON_FALSE(fp, ESP_FAIL, TAG, "unable to open file");

    ESP_LOGI(TAG, "Playing '%s'", file_path);
    ESP_RETURN_ON_ERROR(audio_player_play(fp), TAG, "audio_player_play failed");

    memcpy(audio_file_path, file_path, sizeof(audio_file_path));

    return ESP_OK;
}

void bsp_extra_player_register_callback(audio_player_cb_t cb, void *user_data)
{
    audio_idle_callback = cb;
    audio_idle_cb_user_data = user_data;
}

bool bsp_extra_player_is_playing_by_path(const char *file_path)
{
    return (strcmp(audio_file_path, file_path) == 0);
}

bool bsp_extra_player_is_playing_by_index(file_iterator_instance_t *instance, int index)
{
    return (index == file_iterator_get_index(instance));
}