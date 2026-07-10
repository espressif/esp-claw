/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_MODULE_ENVIRONMENTAL_SENSOR_SELECTED_CHIP_NAME "bme690"

int luaopen_environmental_sensor(lua_State *L);
esp_err_t lua_module_environmental_sensor_register(void);

#ifdef __cplusplus
}
#endif
