/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase-1 display sleep: backlight off + LVGL pause + optional platform hooks.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start monitor task; register touch wake. Call after display + touch are up. */
esp_err_t power_manager_init(void);

/** User adjusted backlight (e.g. settings slider); used when waking the panel. */
void power_manager_set_backlight_percent(int brightness_percent);

/** Reset idle timer (touch / explicit UI activity). */
void power_manager_notify_activity(void);

/** Prevent auto screen-off (upload, print, …). Must be paired with unblock. */
void power_manager_block_sleep(const char *tag);

void power_manager_unblock_sleep(const char *tag);

bool power_manager_is_screen_on(void);

/**
 * Called when entering / leaving screen-off.
 * Override in application (strong symbol) to stop camera preview, etc.
 */
void power_manager_on_screen_off(void);
void power_manager_on_screen_on(void);

#ifdef __cplusplus
}
#endif
