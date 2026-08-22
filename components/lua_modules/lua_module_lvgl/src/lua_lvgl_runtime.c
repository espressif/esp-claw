/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static const char *TAG = "lua_lvgl";
lua_lvgl_state_t s_lvgl;

esp_err_t lua_lvgl_lock(void)
{
    esp_err_t err;

    if (display_service_is_started()) {
        err = display_service_lock();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "display service lock failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (!s_lvgl.mutex) {
        s_lvgl.mutex = xSemaphoreCreateMutex();
    }
    if (!s_lvgl.mutex) {
        if (display_service_is_started()) {
            display_service_unlock();
        }
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_lvgl.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        if (display_service_is_started()) {
            display_service_unlock();
        }
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void lua_lvgl_unlock(void)
{
    if (s_lvgl.mutex) {
        xSemaphoreGive(s_lvgl.mutex);
    }
    if (display_service_is_started()) {
        display_service_unlock();
    }
}

int lua_lvgl_error_esp(lua_State *L, const char *what, esp_err_t err)
{
    return luaL_error(L, "lvgl %s failed: %s", what, esp_err_to_name(err));
}

static esp_err_t lua_lvgl_quiesce_runtime(void)
{
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) {
        return err;
    }
    if (s_lvgl.runtime_initialized) {
        s_lvgl.runtime_initialized = false;
        lua_lvgl_indev_release_locked();
        /* The Lua LVGL session shares the global LVGL runtime with system UI. Do not call lv_anim_delete_all() here; object-bound animations are cleaned up when their objects are deleted. */
    }
    lua_lvgl_unlock();
    return ESP_OK;
}

static void lua_lvgl_drain_event_queue_locked(void)
{
    /* After lv_display_delete + invalidate_records, every record's events
     * list has already been emptied via record_release_resources. Anything
     * still sitting in the event queue is a `dead` sub waiting for the
     * script task to free it. We do that here, while still on the script
     * task and still holding the lock. */
    while (s_lvgl.event_queue_head) {
        lua_lvgl_event_sub_t *sub = s_lvgl.event_queue_head;

        s_lvgl.event_queue_head = sub->queue_next;
        sub->queue_next = NULL;
        sub->queued = false;
        lua_lvgl_queue_pending_unref_locked(sub->callback_ref);
        sub->callback_ref = LUA_NOREF;
        free(sub);
    }
    s_lvgl.event_queue_tail = NULL;
}

static void lua_lvgl_delete_owned_objects_locked(void)
{
    if (s_lvgl.root_screen != NULL && lv_obj_is_valid(s_lvgl.root_screen)) {
        lv_obj_delete(s_lvgl.root_screen);
        s_lvgl.root_screen = NULL;
    }

    for (lua_lvgl_obj_record_t *record = s_lvgl.records; record != NULL; record = record->next) {
        if (record->generation == s_lvgl.generation &&
                record->owned &&
                record->valid &&
                record->obj != NULL &&
                lv_obj_is_valid(record->obj)) {
            lv_obj_delete(record->obj);
        }
    }
}

static void lua_lvgl_release_runtime_locked(void)
{
    /* Snapshot the owner before deleting LVGL objects: LV_EVENT_DELETE fills
     * pending_unrefs, and those refs must be drained against the live Lua
     * state while we are still on the script task. */
    lua_State *owner = s_lvgl.runtime_owner;

    lua_lvgl_indev_release_locked();
    lua_lvgl_delete_owned_objects_locked();
    lua_lvgl_invalidate_records_locked();
    lua_lvgl_release_fonts_locked();
    lua_lvgl_destroy_default_font_locked();
    lua_lvgl_drain_event_queue_locked();
    lua_lvgl_drain_pending_unrefs_locked(owner);
    heap_caps_free(s_lvgl.draw_buf);
    s_lvgl.draw_buf = NULL;
    s_lvgl.draw_buf_size = 0;
    s_lvgl.panel = NULL;
    s_lvgl.io = NULL;
    s_lvgl.width = 0;
    s_lvgl.height = 0;
    s_lvgl.panel_if = LUA_MODULE_LVGL_PANEL_IF_IO;
    s_lvgl.display = NULL;
    s_lvgl.root_screen = NULL;
    s_lvgl.runtime_initialized = false;
    s_lvgl.runtime_owner = NULL;
    s_lvgl.flush_callbacks_registered = false;
    s_lvgl.flush_pending = false;
    s_lvgl.generation++;
}

static void lua_lvgl_session_cleanup_cb(display_service_session_handle_t session, void *user_ctx)
{
    (void)session;
    (void)user_ctx;

    if (s_lvgl.mutex && xSemaphoreTake(s_lvgl.mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "lvgl session cleanup lock timeout");
        return;
    }
    lua_lvgl_release_runtime_locked();
    if (s_lvgl.mutex) {
        xSemaphoreGive(s_lvgl.mutex);
    }
}

esp_err_t lua_lvgl_deinit_runtime(void)
{
    esp_err_t err;
    display_service_session_handle_t session = s_lvgl.display_session;

    err = lua_lvgl_quiesce_runtime();
    if (err != ESP_OK) {
        return err;
    }

    if (session != NULL) {
        err = display_service_close(session);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "display session close failed: %s", esp_err_to_name(err));
            return err;
        }
        s_lvgl.display_session = NULL;
    } else {
        err = lua_lvgl_lock();
        if (err != ESP_OK) {
            return err;
        }
        lua_lvgl_release_runtime_locked();
        lua_lvgl_unlock();
    }
    return ESP_OK;
}
static int lua_lvgl_init(lua_State *L)
{
    int buffer_lines = LUA_MODULE_LVGL_DEFAULT_BUFFER_LINES;
    int tick_ms = LUA_MODULE_LVGL_DEFAULT_TICK_MS;
    int task_period_ms = LUA_MODULE_LVGL_DEFAULT_TASK_PERIOD_MS;
    int font_size = LUA_MODULE_LVGL_DEFAULT_FONT_SIZE;
    int font_cache_size = LV_TINY_TTF_CACHE_GLYPH_CNT;
    int opts_index = 0;
    const char *font_path = LUA_MODULE_LVGL_DEFAULT_FONT_PATH;
    char font_path_buf[LUA_MODULE_LVGL_PATH_MAX];
    lv_display_t *display = NULL;
    lv_obj_t *root_screen = NULL;
    display_service_session_handle_t session = NULL;
    esp_err_t err;

    font_path_buf[0] = '\0';
    if (lua_lvgl_opt_table(L, 6)) {
        opts_index = 6;
        buffer_lines = lua_lvgl_get_opt_int_field(L, 6, "buffer_lines", buffer_lines);
        tick_ms = lua_lvgl_get_opt_int_field(L, 6, "tick_ms", tick_ms);
        task_period_ms = lua_lvgl_get_opt_int_field(L, 6, "task_period_ms", task_period_ms);
    } else if (lua_lvgl_opt_table(L, 1)) {
        opts_index = 1;
        buffer_lines = lua_lvgl_get_opt_int_field(L, 1, "buffer_lines", buffer_lines);
        tick_ms = lua_lvgl_get_opt_int_field(L, 1, "tick_ms", tick_ms);
        task_period_ms = lua_lvgl_get_opt_int_field(L, 1, "task_period_ms", task_period_ms);
    }
    if (opts_index > 0) {
        font_path = lua_lvgl_get_opt_string_field(L, opts_index, "font_path");
        if (!font_path || !font_path[0]) {
            font_path = LUA_MODULE_LVGL_DEFAULT_FONT_PATH;
        }
        int written = snprintf(font_path_buf, sizeof(font_path_buf), "%s", font_path);
        if (written <= 0 || (size_t)written >= sizeof(font_path_buf)) {
            ESP_LOGE(TAG, "default font path too long");
            return luaL_error(L, "lvgl option 'font_path' is too long");
        }
        font_path = font_path_buf;
        font_size = lua_lvgl_get_opt_int_field(L, opts_index, "font_size", font_size);
        font_cache_size = lua_lvgl_get_opt_int_field(L, opts_index, "font_cache_size", font_cache_size);
    }
    luaL_argcheck(L, buffer_lines > 0, 1, "buffer_lines must be positive");
    luaL_argcheck(L, tick_ms > 0, 6, "tick_ms must be positive");
    luaL_argcheck(L, task_period_ms > 0, 6, "task_period_ms must be positive");
    if (font_size <= 0) {
        ESP_LOGE(TAG, "invalid default font size: %d", font_size);
        return luaL_error(L, "lvgl option 'font_size' must be positive");
    }
    if (font_cache_size < 0) {
        ESP_LOGE(TAG, "invalid default font cache size: %d", font_cache_size);
        return luaL_error(L, "lvgl option 'font_cache_size' must be non-negative");
    }

    if (!s_lvgl.mutex) {
        s_lvgl.mutex = xSemaphoreCreateMutex();
        if (!s_lvgl.mutex) {
            return lua_lvgl_error_esp(L, "create mutex", ESP_ERR_NO_MEM);
        }
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is already initialized");
    }
    lua_lvgl_unlock();

    err = display_service_open(&(display_service_session_config_t) {
        .owner_name = "lua_lvgl",
        .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
        .flags = DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE |
                 DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY,
        .cleanup_cb = lua_lvgl_session_cleanup_cb,
        .display_config = {
            .buffer_lines = (uint32_t)buffer_lines,
            .tick_ms = (uint32_t)tick_ms,
            .task_period_ms = (uint32_t)task_period_ms,
        },
    }, &session);
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "open display session", err);
    }
    display = display_service_session_display(session);
    if (display == NULL) {
        (void)display_service_close(session);
        return luaL_error(L, "display service LVGL display is NULL");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        (void)display_service_close(session);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    err = lua_lvgl_register_fs_locked();
    if (err != ESP_OK) {
        lua_lvgl_unlock();
        (void)display_service_close(session);
        return lua_lvgl_error_esp(L, "register fs", err);
    }
    err = lua_lvgl_create_default_font_locked(font_path, (uint32_t)font_size, (uint32_t)font_cache_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "default font unavailable, using LVGL built-in font");
    }

    root_screen = lv_obj_create(NULL);
    if (root_screen == NULL) {
        lua_lvgl_destroy_default_font_locked();
        lua_lvgl_unlock();
        (void)display_service_close(session);
        return luaL_error(L, "lvgl root screen create failed");
    }
    lua_lvgl_apply_default_font_locked(root_screen);
    err = display_service_session_load_screen_locked(session, root_screen);
    if (err != ESP_OK) {
        lv_obj_delete(root_screen);
        lua_lvgl_destroy_default_font_locked();
        lua_lvgl_unlock();
        (void)display_service_close(session);
        return lua_lvgl_error_esp(L, "load root screen", err);
    }

    s_lvgl.display_session = session;
    s_lvgl.width = (int)lv_display_get_horizontal_resolution(display);
    s_lvgl.height = (int)lv_display_get_vertical_resolution(display);
    s_lvgl.tick_ms = (uint32_t)tick_ms;
    s_lvgl.task_period_ms = (uint32_t)task_period_ms;
    s_lvgl.display = display;
    s_lvgl.root_screen = root_screen;
    s_lvgl.runtime_initialized = true;
    s_lvgl.runtime_owner = L;
    s_lvgl.task_stop = false;
    lua_lvgl_unlock();

    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_deinit(lua_State *L)
{
    esp_err_t err;

    if (s_lvgl.runtime_initialized && s_lvgl.runtime_owner != L) {
        return luaL_error(L, "lvgl runtime is owned by another Lua runtime");
    }

    err = lua_lvgl_deinit_runtime();

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "deinit", err);
    }
    lua_pushboolean(L, 1);
    return 1;
}
void lua_lvgl_exit_cleanup(lua_State *L)
{
    /* Single-script subsystem: only the runtime owner triggers a deinit on
     * exit. Non-owner Lua states are expected to never create LVGL objects
     * per the single-script rule (see RFC-single-script-ui.md), so no
     * per-state object cleanup is performed here. */
    if (!L) {
        return;
    }
    if (s_lvgl.runtime_initialized && s_lvgl.runtime_owner == L) {
        ESP_LOGI(TAG, "Lua exit cleanup: deinitializing lvgl owned by exiting state");
        (void)lua_lvgl_deinit_runtime();
    }
}

const luaL_Reg lua_lvgl_runtime_funcs[] = {
    {"init", lua_lvgl_init},
    {"deinit", lua_lvgl_deinit},
    {NULL, NULL},
};
