/*
 * SPDX-FileCopyrightText: 2025
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool parent_net_sta_has_ip(void);
bool parent_net_http_allowed(void);
void parent_reset_nvs_boot_log(void);
void parent_reset_log_phase(const char *phase);

#ifdef __cplusplus
}
#endif
