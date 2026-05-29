/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "parent_print_fs.h"

#include <stdio.h>
#include <sys/stat.h>

#include "bsp/esp-bsp.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"

#define PARENT_PRINT_RESERVE (512 * 1024)
#define PARENT_PRINT_SPIFFS_RESERVE (65536)

static bool sd_mounted(void)
{
    struct stat st;
    return stat(BSP_SD_MOUNT_POINT, &st) == 0;
}

static bool spiffs_mounted(void)
{
    struct stat st;
    return stat(BSP_SPIFFS_MOUNT_POINT, &st) == 0;
}

bool parent_print_fs_is_mounted(void)
{
    return sd_mounted() || spiffs_mounted();
}

bool parent_print_fs_has_space_for(size_t file_bytes)
{
    if (sd_mounted()) {
        uint64_t total = 0;
        uint64_t free_b = 0;
        if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_b) != ESP_OK) {
            return false;
        }
        const uint64_t need = (uint64_t)file_bytes + (uint64_t)PARENT_PRINT_RESERVE;
        return free_b >= need;
    }
    if (spiffs_mounted()) {
        size_t total = 0;
        size_t used = 0;
        if (esp_spiffs_info(CONFIG_BSP_SPIFFS_PARTITION_LABEL, &total, &used) != ESP_OK) {
            return false;
        }
        return total >= used + file_bytes + PARENT_PRINT_SPIFFS_RESERVE;
    }
    return false;
}

esp_err_t parent_print_fs_make_part_path(int print_id, char *out, size_t out_cap)
{
    if (out == NULL || out_cap < 16 || print_id < 1) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *base = sd_mounted() ? BSP_SD_MOUNT_POINT : BSP_SPIFFS_MOUNT_POINT;
    if (!sd_mounted() && !spiffs_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }
    int n = snprintf(out, out_cap, "%s/.print_%d.part", base, print_id);
    if (n <= 0 || (size_t)n >= out_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}
