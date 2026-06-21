/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_match_watch.h"

#include "cap_lua.h"
#include "esp_err.h"
#include "lauxlib.h"
#include "match_watch_runtime.h"

static int lua_match_watch_push_result(lua_State *L, esp_err_t ret)
{
    if (ret == ESP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }

    return luaL_error(L, "%s", esp_err_to_name(ret));
}

static int lua_match_watch_open(lua_State *L)
{
    return lua_match_watch_push_result(L, match_watch_runtime_open(NULL, NULL));
}

static int lua_match_watch_close(lua_State *L)
{
    return lua_match_watch_push_result(L, match_watch_runtime_close());
}

static int lua_match_watch_set_favorite(lua_State *L)
{
    const char *team = NULL;
    const char *competition = NULL;
    esp_err_t ret;

    if (!lua_isnoneornil(L, 1)) {
        team = luaL_checkstring(L, 1);
    }
    if (!lua_isnoneornil(L, 2)) {
        competition = luaL_checkstring(L, 2);
    }

    ret = match_watch_runtime_set_favorite(team, competition);
    return lua_match_watch_push_result(L, ret);
}

static int lua_match_watch_set_competition(lua_State *L)
{
    const char *competition = NULL;

    if (!lua_isnoneornil(L, 1)) {
        competition = luaL_checkstring(L, 1);
    }

    return lua_match_watch_push_result(L, match_watch_runtime_set_competition(competition));
}

static int lua_match_watch_set_reminders(lua_State *L)
{
    bool enabled = lua_toboolean(L, 1);

    return lua_match_watch_push_result(L, match_watch_runtime_set_reminders(enabled));
}

static int lua_match_watch_push_data_json(lua_State *L)
{
    const char *json = luaL_checkstring(L, 1);

    return lua_match_watch_push_result(L, match_watch_runtime_push_data_json(json));
}

int luaopen_match_watch(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_match_watch_open);
    lua_setfield(L, -2, "open");
    lua_pushcfunction(L, lua_match_watch_close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, lua_match_watch_set_favorite);
    lua_setfield(L, -2, "set_favorite");
    lua_pushcfunction(L, lua_match_watch_set_competition);
    lua_setfield(L, -2, "set_competition");
    lua_pushcfunction(L, lua_match_watch_set_reminders);
    lua_setfield(L, -2, "set_reminders");
    lua_pushcfunction(L, lua_match_watch_push_data_json);
    lua_setfield(L, -2, "push_data_json");
    return 1;
}

esp_err_t lua_module_match_watch_register(void)
{
    esp_err_t ret;

    ret = match_watch_runtime_provider_set_backend(cap_lua_run_script_async,
                                                   cap_lua_stop_all_jobs);
    if (ret != ESP_OK) {
        return ret;
    }
    return cap_lua_register_module("match_watch", luaopen_match_watch);
}
