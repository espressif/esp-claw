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

int luaopen_thread(lua_State *L);
esp_err_t lua_module_thread_register(void);

int lua_module_thread_push_sync(lua_State *L);
void lua_module_thread_register_job_funcs(lua_State *L);
esp_err_t thread_sync_init(void);

#ifdef __cplusplus
}
#endif
