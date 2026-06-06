/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "wave_rover_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wave_rover_mcp_start(const wave_rover_config_t *cfg);
esp_err_t wave_rover_mcp_stop(void);
bool      wave_rover_mcp_is_running(void);

#ifdef __cplusplus
}
#endif
