/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_album_fs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "bsp/esp-bsp.h"
#include "esp_vfs_fat.h"

bool parent_album_fs_is_mounted(void)
{
    struct stat st;
    return stat(BSP_SD_MOUNT_POINT, &st) == 0;
}

bool parent_album_fs_has_space_for(size_t file_bytes)
{
    if (!parent_album_fs_is_mounted()) {
        return false;
    }
    uint64_t total = 0;
    uint64_t free_b = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_b) != ESP_OK) {
        return false;
    }
    const uint64_t need = (uint64_t)file_bytes + (uint64_t)PARENT_ALBUM_RESERVE;
    return free_b >= need;
}

static bool ends_with_jpeg(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) {
        return false;
    }
    return strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0;
}

bool parent_album_fs_sanitize_name(const char *in, char *out, size_t out_cap)
{
    if (out == NULL || out_cap < 8) {
        return false;
    }
    out[0] = '\0';
    if (in == NULL || in[0] == '\0') {
        return false;
    }
    const char *base = in;
    const char *slash = strrchr(in, '/');
    if (slash != NULL) {
        base = slash + 1;
    }
    if (strstr(base, "..") != NULL) {
        return false;
    }
    size_t len = strlen(base);
    if (len == 0 || len >= PARENT_ALBUM_FILENAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = base[i];
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    if (!ends_with_jpeg(base)) {
        return false;
    }
    snprintf(out, out_cap, "%s", base);
    return true;
}

static void make_timestamp_name(char *out, size_t cap)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    snprintf(out, cap, "parent_%04d%02d%02d_%02d%02d%02d.jpg",
             tm_info.tm_year + 1900,
             tm_info.tm_mon + 1,
             tm_info.tm_mday,
             tm_info.tm_hour,
             tm_info.tm_min,
             tm_info.tm_sec);
}

esp_err_t parent_album_fs_pick_final_path(const char *basename_or_null, char *out, size_t out_cap)
{
    if (out == NULL || out_cap < 16) {
        return ESP_ERR_INVALID_ARG;
    }
    char name[PARENT_ALBUM_FILENAME_MAX];
    if (basename_or_null != NULL && basename_or_null[0] != '\0') {
        if (!parent_album_fs_sanitize_name(basename_or_null, name, sizeof(name))) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        make_timestamp_name(name, sizeof(name));
    }
    for (int suffix = 0; suffix < 1000; suffix++) {
        char try_name[PARENT_ALBUM_FILENAME_MAX];
        if (suffix == 0) {
            snprintf(try_name, sizeof(try_name), "%s", name);
        } else {
            const char *dot = strrchr(name, '.');
            if (dot == NULL) {
                return ESP_FAIL;
            }
            snprintf(try_name, sizeof(try_name), "%.*s_%d%s", (int)(dot - name), name, suffix, dot);
        }
        int n = snprintf(out, out_cap, "%s/%s", BSP_SD_MOUNT_POINT, try_name);
        if (n <= 0 || (size_t)n >= out_cap) {
            return ESP_ERR_NO_MEM;
        }
        if (access(out, F_OK) != 0) {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t parent_album_fs_make_part_path(int album_id, char *out, size_t out_cap)
{
    int n = snprintf(out, out_cap, "%s/.album_%d.part", BSP_SD_MOUNT_POINT, album_id);
    if (n <= 0 || (size_t)n >= out_cap) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
