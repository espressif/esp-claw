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

int luaopen_event_publisher(lua_State *L);
esp_err_t lua_module_event_publisher_register(void);

#ifdef __cplusplus
}
#endif
