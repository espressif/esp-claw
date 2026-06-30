/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static int lua_lvgl_image(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_IMAGE);
}

static int lua_lvgl_line(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_LINE);
}

static int lua_lvgl_arc(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_ARC);
}

static int lua_lvgl_spinner(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_SPINNER);
}

static int lua_lvgl_scale(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_SCALE);
}

static int lua_lvgl_checkbox(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_CHECKBOX);
}

static int lua_lvgl_switch(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_SWITCH);
}

static int lua_lvgl_dropdown(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_DROPDOWN);
}

static int lua_lvgl_roller(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_ROLLER);
}

static int lua_lvgl_keyboard(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_KEYBOARD);
}

static int lua_lvgl_list(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_LIST);
}

static int lua_lvgl_textarea(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_TEXTAREA);
}

static int lua_lvgl_table(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_TABLE);
}
int lua_lvgl_list_add_text(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *text = luaL_checkstring(L, 2);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *list;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    list = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!list) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    if (type != LUA_LVGL_OBJ_LIST) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl list:add_text requires a list object");
    }
    obj = lv_list_add_text(list, text);
    if (!lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_LIST_TEXT)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_list_add_button(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *text = luaL_checkstring(L, 2);
    const char *symbol = lua_isnoneornil(L, 3) ? NULL : luaL_checkstring(L, 3);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *list;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    list = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!list) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    if (type != LUA_LVGL_OBJ_LIST) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl list:add_button requires a list object");
    }
    obj = lv_list_add_button(list, symbol, text);
    if (!lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_LIST_BUTTON)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_table_set_cell(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int row = (int)luaL_checkinteger(L, 2);
    int col = (int)luaL_checkinteger(L, 3);
    const char *text = luaL_checkstring(L, 4);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *table;
    const char *obj_error = NULL;

    luaL_argcheck(L, row > 0 && col > 0, 2, "row and col are 1-based and must be positive");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    table = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!table) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    if (type != LUA_LVGL_OBJ_TABLE) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl table:set_cell requires a table object");
    }
    lv_table_set_cell_value(table, (uint32_t)(row - 1), (uint32_t)(col - 1), text);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_table_get_cell(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int row = (int)luaL_checkinteger(L, 2);
    int col = (int)luaL_checkinteger(L, 3);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *table;
    const char *value;
    const char *obj_error = NULL;

    luaL_argcheck(L, row > 0 && col > 0, 2, "row and col are 1-based and must be positive");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    table = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!table) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    if (type != LUA_LVGL_OBJ_TABLE) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl table:get_cell requires a table object");
    }
    value = lv_table_get_cell_value(table, (uint32_t)(row - 1), (uint32_t)(col - 1));
    lua_pushstring(L, value ? value : "");
    lua_lvgl_unlock();
    return 1;
}

/* Only factories are registered on the `lvgl` module table. The list and
 * table compound operations above are exposed as methods on the list/table
 * metatables in lua_lvgl_methods.c. */
const luaL_Reg lua_lvgl_extra_widget_funcs[] = {
    {"image", lua_lvgl_image},
    {"line", lua_lvgl_line},
    {"arc", lua_lvgl_arc},
    {"spinner", lua_lvgl_spinner},
    {"scale", lua_lvgl_scale},
    {"checkbox", lua_lvgl_checkbox},
    {"switch", lua_lvgl_switch},
    {"dropdown", lua_lvgl_dropdown},
    {"roller", lua_lvgl_roller},
    {"keyboard", lua_lvgl_keyboard},
    {"list", lua_lvgl_list},
    {"textarea", lua_lvgl_textarea},
    {"table", lua_lvgl_table},
    {NULL, NULL},
};
