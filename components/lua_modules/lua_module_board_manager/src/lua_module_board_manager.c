/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_board_manager.h"

#include "esp_err.h"
#include "lauxlib.h"
#include "esp_board_manager_includes.h"
#include "cap_lua.h"

static int push_err(lua_State *L, esp_err_t err, const char *msg)
{
    lua_pushnil(L);
    if (msg) {
        lua_pushfstring(L, "%s: %s", msg, esp_err_to_name(err));
    } else {
        lua_pushstring(L, esp_err_to_name(err));
    }
    return 2;
}

static int lua_bm_get_board_info(lua_State *L)
{
    esp_board_info_t info = {0};
    esp_err_t err = esp_board_manager_get_board_info(&info);
    if (err != ESP_OK) {
        return push_err(L, err, "get_board_info");
    }
    lua_newtable(L);
    lua_pushstring(L, info.name         ? info.name         : "");
    lua_setfield(L, -2, "name");
    lua_pushstring(L, info.chip         ? info.chip         : "");
    lua_setfield(L, -2, "chip");
    lua_pushstring(L, info.version      ? info.version      : "");
    lua_setfield(L, -2, "version");
    lua_pushstring(L, info.description  ? info.description  : "");
    lua_setfield(L, -2, "description");
    lua_pushstring(L, info.manufacturer ? info.manufacturer : "");
    lua_setfield(L, -2, "manufacturer");
    return 1;
}

static int lua_bm_get_camera_paths(lua_State *L)
{
#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
    /* Query only: the board owns the camera and its lifetime. */
    dev_camera_handle_t *camera = NULL;
    esp_err_t err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_CAMERA, (void **)&camera);
    if (err != ESP_OK) return push_err(L, err, "get_camera_paths");
    if (camera == NULL || camera->dev_path == NULL) return push_err(L, ESP_ERR_NOT_FOUND, "get_camera_paths");

    lua_newtable(L);
    lua_pushstring(L, camera->dev_path);
    lua_setfield(L, -2, "dev_path");
    if (camera->meta_path != NULL) {
        lua_pushstring(L, camera->meta_path);
        lua_setfield(L, -2, "meta_path");
    }
    return 1;
#else
    return push_err(L, ESP_ERR_NOT_SUPPORTED, "get_camera_paths");
#endif
}

int luaopen_board_manager(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"get_board_info",    lua_bm_get_board_info},
        {"get_camera_paths",  lua_bm_get_camera_paths},
        {NULL, NULL},
    };

    luaL_newlib(L, funcs);
    return 1;
}

esp_err_t lua_module_board_manager_register(void)
{
    return cap_lua_register_module("board_manager", luaopen_board_manager);
}
