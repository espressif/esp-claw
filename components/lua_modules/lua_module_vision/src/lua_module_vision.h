/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

#if CONFIG_LUA_MODULE_VISION_MOTION_DETECT
/**
 * @brief Open the motion_detect Lua module.
 *
 * @param L Lua VM state
 *
 * @return Number of Lua return values
 */
int luaopen_motion_detect(lua_State *L);
#endif

/**
 * @brief Register all vision Lua modules with cap_lua.
 *
 * @return ESP_OK on success, or an ESP error code from cap_lua
 */
esp_err_t lua_module_vision_register(void);

#ifdef __cplusplus
}
#endif
