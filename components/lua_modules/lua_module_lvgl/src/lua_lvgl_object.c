/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"

lua_lvgl_obj_ud_t *lua_lvgl_check_ud(lua_State *L, int index)
{
    /* Accept any of the per-type metatables registered under "lvgl.obj.*"
     * by checking a shared sentinel field on the metatable. This avoids
     * binding lua_lvgl_check_ud to a single metatable name and lets each
     * widget type carry its own method table while still being recognized
     * here uniformly. */
    void *ud = lua_touserdata(L, index);

    if (ud != NULL && lua_getmetatable(L, index)) {
        int found;

        lua_getfield(L, -1, "__lvgl_obj");
        found = lua_toboolean(L, -1);
        lua_pop(L, 2);
        if (found) {
            return (lua_lvgl_obj_ud_t *)ud;
        }
    }
    luaL_argerror(L, index, "lvgl object expected");
    return NULL;
}

/* Returns a live object with the LVGL lock held; the caller must unlock. */
lv_obj_t *lua_lvgl_lock_widget(lua_State *L, lua_lvgl_obj_ud_t *ud, lua_lvgl_obj_type_t expected)
{
    esp_err_t err = lua_lvgl_lock();
    if (err != ESP_OK) lua_lvgl_error_esp(L, "lock", err);
    const char *error = NULL;
    lua_lvgl_obj_type_t type;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, &type, &error);
    if (!obj || type != expected) {
        lua_lvgl_unlock();
        luaL_error(L, "%s", obj ? "lvgl widget type mismatch" : error);
    }
    return obj;
}

void lua_lvgl_record_release_resources(lua_lvgl_obj_record_t *record)
{
    if (!record) {
        return;
    }
    /* Event subs may be queued for dispatch on the script task, so we must
     * not luaL_unref here directly (callers include LVGL task paths). The
     * helper below pushes refs to s_lvgl.pending_unrefs which is drained
     * from script-task code paths (process_events / run / deinit). */
    lua_lvgl_record_release_events_locked(record);
    free(record->line_points);
    record->line_points = NULL;
    record->line_point_count = 0;
    free(record->grid_cols);
    record->grid_cols = NULL;
    free(record->grid_rows);
    record->grid_rows = NULL;
    if (record->string_array) {
        for (size_t i = 0; i < record->string_array_count; i++) {
            free(record->string_array[i]);
        }
        free(record->string_array);
        record->string_array = NULL;
        record->string_array_count = 0;
    }
    if (record->type == LUA_LVGL_OBJ_IMAGE && record->data2) lv_image_cache_drop(record->data2);
    free(record->data);
    record->data = NULL;
    record->data_size = 0;
    record->data_width = 0;
    record->data_height = 0;
    free(record->data2);
    record->data2 = NULL;
    record->data2_size = 0;
    record->aux_obj = NULL;
}

static void lua_lvgl_obj_delete_event_cb(lv_event_t *event)
{
    lua_lvgl_obj_record_t *record = (lua_lvgl_obj_record_t *)lv_event_get_user_data(event);

    if (!record || lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    lua_lvgl_record_release_resources(record);
    record->obj = NULL;
    record->valid = false;
}

lv_obj_t *lua_lvgl_validate_ud_locked(const lua_lvgl_obj_ud_t *ud,
                                             lua_lvgl_obj_type_t *out_type,
                                             const char **out_error)
{
    lua_lvgl_obj_record_t *record = ud ? ud->record : NULL;

    if (!s_lvgl.runtime_initialized) {
        *out_error = "lvgl runtime is not initialized";
        return NULL;
    }
    if (!record || record->generation != s_lvgl.generation) {
        *out_error = "lvgl object belongs to an old runtime";
        return NULL;
    }
    if (!record->valid || !record->obj) {
        *out_error = "lvgl object has been deleted";
        return NULL;
    }
    if (!lv_obj_is_valid(record->obj)) {
        record->obj = NULL;
        lua_lvgl_record_release_resources(record);
        record->valid = false;
        *out_error = "lvgl object is no longer valid";
        return NULL;
    }
    if (out_type) {
        *out_type = record->type;
    }
    return record->obj;
}

static void lua_lvgl_record_unref(lua_lvgl_obj_record_t *record)
{
    if (atomic_fetch_sub(&record->refs, 1) == 1) free(record);
}

void lua_lvgl_sweep_records_locked(void)
{
    lua_lvgl_obj_record_t **link = &s_lvgl.records;

    while (*link) {
        lua_lvgl_obj_record_t *record = *link;
        if (record->valid) {
            link = &record->next;
        } else {
            *link = record->next;
            record->next = NULL;
            lua_lvgl_record_unref(record);
        }
    }
}

static int lua_lvgl_new_obj_ud(lua_State *L)
{
    lua_lvgl_obj_type_t type = (lua_lvgl_obj_type_t)lua_tointeger(L, 1);
    lua_lvgl_obj_ud_t *ud = (lua_lvgl_obj_ud_t *)lua_newuserdata(L, sizeof(*ud));
    ud->record = NULL;
    luaL_getmetatable(L, lua_lvgl_metatable_for_type(type));
    lua_setmetatable(L, -2);
    return 1;
}

lua_lvgl_obj_ud_t *lua_lvgl_push_obj_ex(lua_State *L, lv_obj_t *obj, lua_lvgl_obj_type_t type, bool owned)
{
    if (!obj) return NULL;
    /* Allocation may trigger GC or fail while the caller holds the LVGL lock. */
    lua_pushcfunction(L, lua_lvgl_new_obj_ud);
    lua_pushinteger(L, type);
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        lua_pop(L, 1);
        return NULL;
    }
    lua_lvgl_obj_ud_t *ud = lua_touserdata(L, -1);

    for (lua_lvgl_obj_record_t *record = s_lvgl.records; record; record = record->next) {
        if (record->valid && record->obj == obj && record->generation == s_lvgl.generation) {
            atomic_fetch_add(&record->refs, 1);
            record->owned |= owned;
            ud->record = record;
            return ud;
        }
    }
    lua_lvgl_obj_record_t *record = calloc(1, sizeof(*record));
    if (!record) return NULL;
    if (!lv_obj_add_event_cb(obj, lua_lvgl_obj_delete_event_cb, LV_EVENT_DELETE, record)) {
        free(record);
        return NULL;
    }
    atomic_init(&record->refs, 2);
    record->obj = obj;
    record->generation = s_lvgl.generation;
    record->type = type;
    record->valid = true;
    record->owned = owned;
    record->next = s_lvgl.records;
    s_lvgl.records = record;
    ud->record = record;
    return ud;
}

lua_lvgl_obj_ud_t *lua_lvgl_push_obj(lua_State *L, lv_obj_t *obj, lua_lvgl_obj_type_t type)
{
    return lua_lvgl_push_obj_ex(L, obj, type, true);
}
void lua_lvgl_invalidate_records_locked(void)
{
    lua_lvgl_obj_record_t *record;

    for (record = s_lvgl.records; record != NULL; record = record->next) {
        if (record->generation == s_lvgl.generation) {
            if (record->valid && record->obj && lv_obj_is_valid(record->obj)) {
                lv_obj_remove_event_cb_with_user_data(record->obj, lua_lvgl_obj_delete_event_cb, record);
            }
            lua_lvgl_record_release_resources(record);
            record->obj = NULL;
            record->valid = false;
        }
    }
}

int lua_lvgl_delete(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_obj_record_t *record = ud->record;
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (s_lvgl.runtime_initialized &&
            record &&
            record->generation == s_lvgl.generation &&
            record->valid &&
            record->obj &&
            lv_obj_is_valid(record->obj)) {
        lv_obj_delete(record->obj);
    }
    if (record) {
        lua_lvgl_record_release_resources(record);
        record->obj = NULL;
        record->valid = false;
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_clean(lua_State *L)
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
    lv_obj_clean(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static lv_obj_t *lock_object(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    esp_err_t err = lua_lvgl_lock();
    if (err != ESP_OK) lua_lvgl_error_esp(L, "lock", err);
    const char *error = NULL;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, NULL, &error);
    if (!obj) {
        lua_lvgl_unlock();
        luaL_error(L, "%s", error);
    }
    return obj;
}

static int set_object_flag(lua_State *L, bool enabled)
{
    static const char *const names[] = {"CLICKABLE", "PRESS_LOCK", "SCROLLABLE", "HIDDEN", NULL};
    static const lv_obj_flag_t flags[] = {LV_OBJ_FLAG_CLICKABLE, LV_OBJ_FLAG_PRESS_LOCK, LV_OBJ_FLAG_SCROLLABLE, LV_OBJ_FLAG_HIDDEN};
    /* Parse before locking: Lua argument errors unwind the C stack. */
    int index = luaL_checkoption(L, 2, NULL, names);
    lv_obj_t *obj = lock_object(L);
    if (enabled) lv_obj_add_flag(obj, flags[index]);
    else lv_obj_remove_flag(obj, flags[index]);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_add_flag(lua_State *L)
{
    return set_object_flag(L, true);
}

int lua_lvgl_clear_flag(lua_State *L)
{
    return set_object_flag(L, false);
}

int lua_lvgl_move_foreground(lua_State *L)
{
    lv_obj_t *obj = lock_object(L);
    if (!lv_obj_get_parent(obj)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl move_foreground requires a child object");
    }
    lv_obj_move_foreground(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_obj_gc(lua_State *L)
{
    /* __gc is only ever invoked by Lua on userdata whose metatable we set
     * to one of the "lvgl.obj.*" tables in lua_lvgl_push_obj, so the cast
     * is unconditionally safe. We avoid lua_lvgl_check_ud here because it
     * raises a Lua error on failure, which is undesirable inside __gc. */
    lua_lvgl_obj_ud_t *ud = (lua_lvgl_obj_ud_t *)lua_touserdata(L, 1);

    if (ud && ud->record) {
        lua_lvgl_obj_record_t *record = ud->record;
        ud->record = NULL;
        /* Lua may collect while the LVGL lock is held. Never lock in GC. */
        lua_lvgl_record_unref(record);
    }
    return 0;
}
