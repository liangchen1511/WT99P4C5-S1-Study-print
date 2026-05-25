/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Pause SDIO background HTTP until IP stable; register WiFi event hooks. */
void camera_power_bridge_init(void);

#ifdef __cplusplus
}
#endif
