/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

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
    free(record->data);
    record->data = NULL;
    record->data_size = 0;
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
        record->valid = false;
        *out_error = "lvgl object is no longer valid";
        return NULL;
    }
    if (out_type) {
        *out_type = record->type;
    }
    return record->obj;
}

lua_lvgl_obj_ud_t *lua_lvgl_push_obj(lua_State *L, lv_obj_t *obj, lua_lvgl_obj_type_t type)
{
    lua_lvgl_obj_record_t *record = (lua_lvgl_obj_record_t *)calloc(1, sizeof(*record));
    lua_lvgl_obj_ud_t *ud;

    if (!record) {
        return NULL;
    }
    ud = (lua_lvgl_obj_ud_t *)lua_newuserdata(L, sizeof(*ud));
    record->obj = obj;
    record->generation = s_lvgl.generation;
    record->type = type;
    record->valid = true;
    record->next = s_lvgl.records;
    s_lvgl.records = record;
    ud->record = record;
    lv_obj_add_event_cb(obj, lua_lvgl_obj_delete_event_cb, LV_EVENT_DELETE, record);
    luaL_getmetatable(L, lua_lvgl_metatable_for_type(type));
    lua_setmetatable(L, -2);
    return ud;
}
void lua_lvgl_invalidate_records_locked(void)
{
    lua_lvgl_obj_record_t *record;

    for (record = s_lvgl.records; record != NULL; record = record->next) {
        if (record->generation == s_lvgl.generation) {
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

int lua_lvgl_obj_gc(lua_State *L)
{
    /* __gc is only ever invoked by Lua on userdata whose metatable we set
     * to one of the "lvgl.obj.*" tables in lua_lvgl_push_obj, so the cast
     * is unconditionally safe. We avoid lua_lvgl_check_ud here because it
     * raises a Lua error on failure, which is undesirable inside __gc. */
    lua_lvgl_obj_ud_t *ud = (lua_lvgl_obj_ud_t *)lua_touserdata(L, 1);

    if (ud != NULL) {
        ud->record = NULL;
    }
    return 0;
}
