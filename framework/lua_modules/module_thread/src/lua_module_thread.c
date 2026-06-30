/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_thread.h"

#include "cap_lua.h"
#include "lauxlib.h"

#define LUA_MODULE_THREAD_NAME "thread"

int luaopen_thread(lua_State *L)
{
    if (thread_sync_init() != ESP_OK) {
        luaL_error(L, "thread.sync: failed to create registry lock");
    }

    lua_newtable(L);
    lua_module_thread_register_job_funcs(L);

    lua_module_thread_push_sync(L);
    lua_setfield(L, -2, "sync");

    return 1;
}

esp_err_t lua_module_thread_register(void)
{
    esp_err_t err = thread_sync_init();

    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_module(LUA_MODULE_THREAD_NAME, luaopen_thread);
}
