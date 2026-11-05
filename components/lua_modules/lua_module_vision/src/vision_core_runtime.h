/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lua_vision_core_runtime_init(void);
esp_err_t lua_vision_core_lock(void);
void lua_vision_core_unlock(void);

#ifdef __cplusplus
}
#endif
