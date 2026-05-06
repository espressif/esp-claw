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

/**
 * @brief Register HTTP client + server Lua modules
 *
 * @return ESP_OK on success
 */
esp_err_t lua_module_http_register(void);

/**
 * @brief Lua binding entry: luaopen_http (client)
 */
int luaopen_http(lua_State *L);

/**
 * @brief Lua binding entry: luaopen_http_server
 */
int luaopen_http_server(lua_State *L);

#ifdef __cplusplus
}
#endif
