/*
 * SPDX-License-Identifier: Apache-2.0
 * Boot splash (LVGL timeline animation).
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Visible animation length (ms), excluding optional fade-out tail. */
#define BOOT_SPLASH_DURATION_MS 3000

/** Create splash layers on @p parent (usually active screen). */
void boot_splash_show(lv_obj_t *parent);

/**
 * Play animation (~ @ref BOOT_SPLASH_DURATION_MS). Call with @c bsp_display_lock held;
 * unlocks LVGL while waiting so the LVGL task can run tweens.
 */
void boot_splash_run(void);

/** Remove splash objects (safe if already removed). */
void boot_splash_dismiss(void);

#ifdef __cplusplus
}
#endif
