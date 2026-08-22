/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static const char *TAG = "lua_lvgl_eaf";

static lv_obj_t *lua_lvgl_eaf_validate_locked(const lua_lvgl_obj_ud_t *ud, lua_lvgl_obj_type_t *out_type, const char **out_error)
{
    lua_lvgl_obj_type_t type;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, &type, out_error);

    if (!obj) {
        return NULL;
    }
    if (type != LUA_LVGL_OBJ_EAF) {
        if (out_error) {
            *out_error = "lvgl eaf method requires an eaf object";
        }
        if (out_type) {
            *out_type = type;
        }
        return NULL;
    }
    if (out_type) {
        *out_type = type;
    }
    return obj;
}

static void lua_lvgl_eaf_release_data(lua_lvgl_obj_record_t *record)
{
    if (!record) {
        return;
    }
    free(record->data);
    record->data = NULL;
    record->data_size = 0;
}

static void *lua_lvgl_eaf_copy_src_data(lua_State *L, int index, size_t *out_len)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, index, &len);
    void *copy;

    luaL_argcheck(L, len > 0, index, "src_data must not be empty");
    copy = malloc(len);
    if (!copy) {
        ESP_LOGE(TAG, "eaf src_data allocation failed: %u bytes", (unsigned)len);
        luaL_error(L, "lvgl eaf src_data allocation failed");
    }
    memcpy(copy, data, len);
    *out_len = len;
    return copy;
}

#define LUA_LVGL_EAF_METHOD_BEGIN() \
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1); \
    esp_err_t err = lua_lvgl_lock(); \
    lv_obj_t *obj; \
    const char *obj_error = NULL; \
    if (err != ESP_OK) { \
        return lua_lvgl_error_esp(L, "lock", err); \
    } \
    obj = lua_lvgl_eaf_validate_locked(ud, NULL, &obj_error); \
    if (!obj) { \
        lua_lvgl_unlock(); \
        return luaL_error(L, "%s", obj_error); \
    }

static int lua_lvgl_eaf(lua_State *L)
{
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_opts_t opts;
    lv_align_t align;
    esp_err_t err;
    lv_obj_t *parent;
    lv_obj_t *obj;
    lua_lvgl_obj_ud_t *created_ud;
    const char *obj_error = NULL;
    void *src_data_copy = NULL;
    size_t src_data_size = 0;

    lua_lvgl_parse_opts(L, 2, &opts);
    if (opts.align_value && lua_lvgl_parse_align(L, opts.align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }
    if (opts.has_opts) {
        lua_getfield(L, 2, "src_data");
        if (!lua_isnil(L, -1)) {
            src_data_copy = lua_lvgl_eaf_copy_src_data(L, -1, &src_data_size);
        }
        lua_pop(L, 1);
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        free(src_data_copy);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        free(src_data_copy);
        return luaL_error(L, "%s", obj_error);
    }

    obj = lv_eaf_create(parent);
    if (!obj) {
        ESP_LOGE(TAG, "eaf object create failed");
        lua_lvgl_unlock();
        free(src_data_copy);
        return luaL_error(L, "lvgl eaf create failed");
    }
    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        lua_lvgl_apply_style_opts_locked(L, 2, obj);
    }

    created_ud = lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_EAF);
    if (!created_ud) {
        ESP_LOGE(TAG, "eaf object record allocation failed");
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        free(src_data_copy);
        return luaL_error(L, "lvgl object record allocation failed");
    }

    if (opts.has_opts) {
        /* Source loading must happen before playback options: esp_lv_eaf_player ignores loop/frame setters until the EAF handle exists. */
        if (src_data_copy) {
            lua_lvgl_eaf_release_data(created_ud->record);
            created_ud->record->data = src_data_copy;
            created_ud->record->data_size = src_data_size;
            src_data_copy = NULL;
            lv_eaf_set_src_data(obj, created_ud->record->data, created_ud->record->data_size);
        } else {
            if (lua_lvgl_has_field(L, 2, "src")) {
                lua_lvgl_eaf_release_data(created_ud->record);
                lv_eaf_set_src(obj, lua_lvgl_get_opt_string_field(L, 2, "src"));
            }
        }
        if (lv_eaf_is_loaded(obj)) {
            if (lua_lvgl_has_field(L, 2, "loop_count")) {
                lv_eaf_set_loop_count(obj, lua_lvgl_get_opt_int_field(L, 2, "loop_count", -1));
            }
            if (lua_lvgl_has_field(L, 2, "loop_enabled")) {
                lv_eaf_set_loop_enabled(obj, lua_lvgl_get_opt_bool_field(L, 2, "loop_enabled", true));
            }
            if (lua_lvgl_has_field(L, 2, "frame_delay")) {
                lv_eaf_set_frame_delay(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "frame_delay", 100));
            }
        }
    }

    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_eaf_set_src(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *src = luaL_checkstring(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_eaf_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lua_lvgl_eaf_release_data(ud->record);
    lv_eaf_set_src(obj, src);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_set_src_data(lua_State *L)
{
    size_t len = 0;
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    void *copy = lua_lvgl_eaf_copy_src_data(L, 2, &len);
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        free(copy);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_eaf_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        free(copy);
        return luaL_error(L, "%s", obj_error);
    }
    lua_lvgl_eaf_release_data(ud->record);
    ud->record->data = copy;
    ud->record->data_size = len;
    lv_eaf_set_src_data(obj, ud->record->data, ud->record->data_size);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_restart(lua_State *L)
{
    LUA_LVGL_EAF_METHOD_BEGIN();
    lv_eaf_restart(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_pause(lua_State *L)
{
    LUA_LVGL_EAF_METHOD_BEGIN();
    lv_eaf_pause(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_resume(lua_State *L)
{
    LUA_LVGL_EAF_METHOD_BEGIN();
    lv_eaf_resume(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_is_loaded(lua_State *L)
{
    bool loaded;

    LUA_LVGL_EAF_METHOD_BEGIN();
    loaded = lv_eaf_is_loaded(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_eaf_get_loop_count(lua_State *L)
{
    int32_t value;

    LUA_LVGL_EAF_METHOD_BEGIN();
    value = lv_eaf_get_loop_count(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, value);
    return 1;
}

int lua_lvgl_eaf_set_loop_count(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int32_t count = (int32_t)luaL_checkinteger(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_eaf_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_eaf_set_loop_count(obj, count);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_get_total_frames(lua_State *L)
{
    int32_t value;

    LUA_LVGL_EAF_METHOD_BEGIN();
    value = lv_eaf_get_total_frames(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, value);
    return 1;
}

int lua_lvgl_eaf_get_current_frame(lua_State *L)
{
    int32_t value;

    LUA_LVGL_EAF_METHOD_BEGIN();
    value = lv_eaf_get_current_frame(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, value);
    return 1;
}

int lua_lvgl_eaf_set_frame_delay(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    uint32_t delay = (uint32_t)luaL_checkinteger(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_argcheck(L, delay > 0, 2, "frame_delay must be positive");
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_eaf_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_eaf_set_frame_delay(obj, delay);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_get_frame_delay(lua_State *L)
{
    uint32_t value;

    LUA_LVGL_EAF_METHOD_BEGIN();
    value = lv_eaf_get_frame_delay(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, value);
    return 1;
}

int lua_lvgl_eaf_set_loop_enabled(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    bool enabled = lua_toboolean(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 2, LUA_TBOOLEAN);
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_eaf_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_eaf_set_loop_enabled(obj, enabled);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_eaf_get_loop_enabled(lua_State *L)
{
    bool enabled;

    LUA_LVGL_EAF_METHOD_BEGIN();
    enabled = lv_eaf_get_loop_enabled(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, enabled);
    return 1;
}

const luaL_Reg lua_lvgl_eaf_module_funcs[] = {
    {"eaf", lua_lvgl_eaf},
    {NULL, NULL},
};
