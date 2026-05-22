/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Input device subsystem (P4 of RFC-single-script-ui.md).
 *
 * Scope of this revision: a single LV_INDEV_TYPE_POINTER backed by an
 * esp_lcd_touch_handle_t obtained from board_manager. Encoder and keypad
 * indevs are intentionally deferred to a follow-up PR (the registration
 * surface here accepts a kind string so adding them later is purely
 * additive).
 *
 * Threading model:
 *   - The read callback runs from the LVGL task while it already holds
 *     lua_lvgl_lock() (taken in lua_lvgl_task before lv_timer_handler).
 *     Our mutex is non-recursive, so the read callback MUST NOT take the
 *     lock again. Instead we read the touch handle from the indev's
 *     user_data slot, which is set once during indev_register and never
 *     mutated until indev_release_locked tears the indev down (with the
 *     LVGL task already stopped).
 *
 *   - The esp_lcd_touch handle itself is owned by board_manager; we only
 *     borrow the pointer. Tearing down the LVGL runtime never frees the
 *     handle, so it stays valid for whoever else uses lcd_touch.
 *
 *   - For interrupt-driven touch controllers, calling
 *     esp_lcd_touch_read_data() from the read callback is functionally
 *     equivalent to a polling read. It does add some I2C/SPI traffic at
 *     the LVGL task period (default ~10ms = 100 Hz), but avoids coupling
 *     this module to lua_module_lcd_touch's static interrupt-task table.
 *     Boards that need true interrupt-driven indev should be addressed
 *     by extending esp_lcd_touch with a shared dispatcher.
 */

#include "lua_lvgl_private.h"

#include "esp_lcd_touch.h"

static const char *TAG = "lua_lvgl_indev";

/* --- Internal helpers -------------------------------------------------- */

static void lua_lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)lv_indev_get_user_data(indev);
    esp_lcd_touch_point_data_t point;
    uint8_t point_count = 0;

    if (!tp) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /* Best-effort polling read; ignore the error code so a single I2C
     * glitch does not stall the indev. Coordinates are only consumed
     * when esp_lcd_touch_get_data confirms a touch point is available. */
    (void)esp_lcd_touch_read_data(tp);
    if (esp_lcd_touch_get_data(tp, &point, &point_count, 1) == ESP_OK && point_count > 0) {
        data->point.x = (int32_t)point.x;
        data->point.y = (int32_t)point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Attach a pointer indev backed by `touch_handle`. Caller holds the lock.
 * Returns NULL on success; on failure returns a static error message and
 * leaves s_lvgl.touch_indev / touch_handle untouched. */
static const char *lua_lvgl_indev_attach_touch_locked(void *touch_handle)
{
    lv_indev_t *indev;

    if (s_lvgl.touch_indev) {
        return "lvgl touch indev is already registered";
    }
    indev = lv_indev_create();
    if (!indev) {
        return "lvgl indev create failed";
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lua_lvgl_touch_read_cb);
    lv_indev_set_user_data(indev, touch_handle);
    if (s_lvgl.display) {
        lv_indev_set_display(indev, s_lvgl.display);
    }

    s_lvgl.touch_indev = indev;
    s_lvgl.touch_handle = touch_handle;
    return NULL;
}

/* --- Public C API (called from runtime teardown) ----------------------- */

void lua_lvgl_indev_release_locked(void)
{
    if (s_lvgl.touch_indev) {
        /* The owning Lua handle is just a borrowed pointer; we never
         * free the esp_lcd_touch handle itself. */
        lv_indev_set_user_data(s_lvgl.touch_indev, NULL);
        lv_indev_delete(s_lvgl.touch_indev);
        s_lvgl.touch_indev = NULL;
    }
    s_lvgl.touch_handle = NULL;
}

/* --- Lua entries ------------------------------------------------------- */

static int lua_lvgl_indev_register(lua_State *L)
{
    const char *kind = luaL_checkstring(L, 1);
    void *handle = NULL;
    esp_err_t err;
    const char *attach_err = NULL;
    bool unsupported = false;

    luaL_argcheck(L, lua_islightuserdata(L, 2), 2, "indev handle (light userdata) expected");
    handle = lua_touserdata(L, 2);
    luaL_argcheck(L, handle != NULL, 2, "indev handle must be non-NULL");

    if (!s_lvgl.runtime_initialized) {
        return luaL_error(L, "lvgl runtime is not initialized");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    if (strcmp(kind, "touch") == 0) {
        attach_err = lua_lvgl_indev_attach_touch_locked(handle);
    } else {
        unsupported = true;
    }
    lua_lvgl_unlock();

    if (unsupported) {
        return luaL_error(L, "lvgl unsupported indev kind: %s", kind);
    }
    if (attach_err) {
        return luaL_error(L, "%s", attach_err);
    }
    ESP_LOGI(TAG, "registered %s indev handle=%p", kind, handle);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_indev_unregister(lua_State *L)
{
    const char *kind = luaL_checkstring(L, 1);
    esp_err_t err;
    bool removed = false;
    bool unsupported = false;

    if (!s_lvgl.runtime_initialized) {
        lua_pushboolean(L, 0);
        return 1;
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (strcmp(kind, "touch") == 0) {
        if (s_lvgl.touch_indev) {
            lv_indev_set_user_data(s_lvgl.touch_indev, NULL);
            lv_indev_delete(s_lvgl.touch_indev);
            s_lvgl.touch_indev = NULL;
            s_lvgl.touch_handle = NULL;
            removed = true;
        }
    } else {
        unsupported = true;
    }
    lua_lvgl_unlock();

    if (unsupported) {
        return luaL_error(L, "lvgl unsupported indev kind: %s", kind);
    }
    lua_pushboolean(L, removed);
    return 1;
}

const luaL_Reg lua_lvgl_indev_module_funcs[] = {
    {"indev_register", lua_lvgl_indev_register},
    {"indev_unregister", lua_lvgl_indev_unregister},
    {NULL, NULL},
};
