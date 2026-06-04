/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "rover_demo_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_mcp_start(const rover_demo_settings_t *s);

#ifdef __cplusplus
}
#endif
