/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static void lua_lvgl_register_funcs(lua_State *L, const luaL_Reg *funcs)
{
    for (; funcs && funcs->name; funcs++) {
        lua_pushcfunction(L, funcs->func);
        lua_setfield(L, -2, funcs->name);
    }
}

int luaopen_lvgl(lua_State *L)
{
    /* Build all per-type metatables ("lvgl.obj.<type>") with their
     * inherited base methods. After this, lua_lvgl_push_obj() can find
     * the right metatable for any widget type via lua_lvgl_metatable_for_type().
     */
    lua_lvgl_register_metatables(L);

    /* The `lvgl` module table now hosts only runtime + factory entries.
     * All object operations (set_xxx / get_xxx / delete / clean / load /
     * add_text / set_cell / ...) are invoked as methods on widget userdata. */
    lua_newtable(L);

    lua_lvgl_register_funcs(L, lua_lvgl_runtime_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_core_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_extra_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_complex_widget_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_event_module_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_indev_module_funcs);
    lua_lvgl_register_funcs(L, lua_lvgl_demo_module_funcs);

    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_IO);
    lua_setfield(L, -2, "PANEL_IF_IO");
    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_RGB);
    lua_setfield(L, -2, "PANEL_IF_RGB");
    lua_pushinteger(L, LUA_MODULE_LVGL_PANEL_IF_MIPI_DSI);
    lua_setfield(L, -2, "PANEL_IF_MIPI_DSI");

    return 1;
}

esp_err_t lua_module_lvgl_register(void)
{
    esp_err_t err = cap_lua_register_module(LUA_MODULE_LVGL_NAME, luaopen_lvgl);

    if (err != ESP_OK) {
        return err;
    }
    return cap_lua_register_exit_cleanup(lua_lvgl_exit_cleanup);
}
