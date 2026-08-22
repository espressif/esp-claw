/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

int lua_lvgl_set_text(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *text = luaL_checkstring(L, 2);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    switch (type) {
    case LUA_LVGL_OBJ_LABEL:
    case LUA_LVGL_OBJ_LIST_TEXT:
    case LUA_LVGL_OBJ_LIST_BUTTON:
        lv_label_set_text(obj, text);
        break;
    case LUA_LVGL_OBJ_BUTTON:
        if (!ud->record->aux_obj || !lv_obj_is_valid(ud->record->aux_obj)) {
            ud->record->aux_obj = lv_label_create(obj);
            lv_obj_align(ud->record->aux_obj, LV_ALIGN_CENTER, 0, 0);
        }
        lv_label_set_text(ud->record->aux_obj, text);
        break;
    case LUA_LVGL_OBJ_CHECKBOX:
        lv_checkbox_set_text(obj, text);
        break;
    case LUA_LVGL_OBJ_DROPDOWN:
        lv_dropdown_set_text(obj, text);
        break;
    case LUA_LVGL_OBJ_TEXTAREA:
        lv_textarea_set_text(obj, text);
        break;
    default:
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl set_text does not support this object type");
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_get_pos(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lua_pushinteger(L, lv_obj_get_x(obj));
    lua_pushinteger(L, lv_obj_get_y(obj));
    lua_lvgl_unlock();
    return 2;
}

int lua_lvgl_get_size(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lua_pushinteger(L, lv_obj_get_width(obj));
    lua_pushinteger(L, lv_obj_get_height(obj));
    lua_lvgl_unlock();
    return 2;
}

int lua_lvgl_is_valid(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_obj_record_t *record = ud->record;
    bool valid = false;

    if (lua_lvgl_lock() == ESP_OK) {
        if (s_lvgl.runtime_initialized &&
                record &&
                record->generation == s_lvgl.generation &&
                record->valid &&
                record->obj &&
                lv_obj_is_valid(record->obj)) {
            valid = true;
        } else if (record) {
            record->valid = false;
            record->obj = NULL;
        }
        lua_lvgl_unlock();
    }
    lua_pushboolean(L, valid);
    return 1;
}

int lua_lvgl_get_value(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    switch (type) {
    case LUA_LVGL_OBJ_BAR:
        lua_pushinteger(L, lv_bar_get_value(obj));
        break;
    case LUA_LVGL_OBJ_SLIDER:
        lua_pushinteger(L, lv_slider_get_value(obj));
        break;
    case LUA_LVGL_OBJ_ARC:
        lua_pushinteger(L, lv_arc_get_value(obj));
        break;
    case LUA_LVGL_OBJ_SCALE:
        lua_pushinteger(L, ud->record->value_cache);
        break;
    case LUA_LVGL_OBJ_DROPDOWN:
        lua_pushinteger(L, (lua_Integer)lv_dropdown_get_selected(obj) + 1);
        break;
    case LUA_LVGL_OBJ_ROLLER:
        lua_pushinteger(L, (lua_Integer)lv_roller_get_selected(obj) + 1);
        break;
    case LUA_LVGL_OBJ_CHECKBOX:
    case LUA_LVGL_OBJ_SWITCH:
        lua_pushboolean(L, lv_obj_has_state(obj, LV_STATE_CHECKED));
        break;
    case LUA_LVGL_OBJ_SPINBOX:
        lua_pushinteger(L, lv_spinbox_get_value(obj));
        break;
    default:
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl get_value does not support this object type");
    }
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_set_value(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_obj_type_t type;
    bool anim = lua_toboolean(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    switch (type) {
    case LUA_LVGL_OBJ_BAR:
        lv_bar_set_value(obj, (int32_t)luaL_checkinteger(L, 2), anim ? LV_ANIM_ON : LV_ANIM_OFF);
        break;
    case LUA_LVGL_OBJ_SLIDER:
        lv_slider_set_value(obj, (int32_t)luaL_checkinteger(L, 2), anim ? LV_ANIM_ON : LV_ANIM_OFF);
        break;
    case LUA_LVGL_OBJ_ARC:
        lv_arc_set_value(obj, (int32_t)luaL_checkinteger(L, 2));
        break;
    case LUA_LVGL_OBJ_SCALE:
        ud->record->value_cache = (int)luaL_checkinteger(L, 2);
        break;
    case LUA_LVGL_OBJ_DROPDOWN: {
        int selected = (int)luaL_checkinteger(L, 2);
        lv_dropdown_set_selected(obj, selected > 0 ? (uint32_t)(selected - 1) : 0);
        break;
    }
    case LUA_LVGL_OBJ_ROLLER: {
        int selected = (int)luaL_checkinteger(L, 2);
        lv_roller_set_selected(obj, selected > 0 ? (uint32_t)(selected - 1) : 0, anim ? LV_ANIM_ON : LV_ANIM_OFF);
        break;
    }
    case LUA_LVGL_OBJ_CHECKBOX:
    case LUA_LVGL_OBJ_SWITCH:
        if (lua_toboolean(L, 2)) {
            lv_obj_add_state(obj, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(obj, LV_STATE_CHECKED);
        }
        break;
    case LUA_LVGL_OBJ_SPINBOX:
        lv_spinbox_set_value(obj, (int32_t)luaL_checkinteger(L, 2));
        break;
    default:
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl set_value does not support this object type");
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_set_range(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int min_value = (int)luaL_checkinteger(L, 2);
    int max_value = (int)luaL_checkinteger(L, 3);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_argcheck(L, max_value > min_value, 3, "max must be greater than min");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    switch (type) {
    case LUA_LVGL_OBJ_BAR:
        lv_bar_set_range(obj, min_value, max_value);
        break;
    case LUA_LVGL_OBJ_SLIDER:
        lv_slider_set_range(obj, min_value, max_value);
        break;
    case LUA_LVGL_OBJ_ARC:
        lv_arc_set_range(obj, min_value, max_value);
        break;
    case LUA_LVGL_OBJ_SCALE:
        lv_scale_set_range(obj, min_value, max_value);
        break;
    case LUA_LVGL_OBJ_SPINBOX:
        lv_spinbox_set_range(obj, min_value, max_value);
        break;
    default:
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl set_range does not support this object type");
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_set_pos(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_pos(obj, x, y);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_set_size(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_argcheck(L, w > 0 && h > 0, 2, "width and height must be positive");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_size(obj, w, h);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_align(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *align_value = luaL_checkstring(L, 2);
    int x = (int)luaL_optinteger(L, 3, 0);
    int y = (int)luaL_optinteger(L, 4, 0);
    lv_align_t align;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (lua_lvgl_parse_align(L, align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_align(obj, align, x, y);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

/* These functions are exposed exclusively as methods on per-type metatables
 * built in lua_lvgl_methods.c. They are no longer registered on the `lvgl`
 * module table itself. */
