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

extern "C" void power_manager_on_screen_off(void)
{
    Camera::stopPreviewStreamingIfRunning();
    /* 息屏后减少 SDIO 上并行 HTTP（policy/chat/album），降低 H_SDIO_DRV 不可恢复后整机重启概率。 */
    parent_album_sync_pause(true);
    parent_chat_bg_pause(true);
    parent_policy_poll_pause(true);
}

extern "C" void power_manager_on_screen_on(void)
{
    (void)Camera::ensurePreviewStreaming();
    parent_album_sync_pause(false);
    parent_chat_bg_pause(false);
    parent_policy_poll_pause(false);
}
