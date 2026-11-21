/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "claw_hw_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_claw_cli_start(claw_hw_registry_handle_t registry);

#ifdef __cplusplus
}
#endif
