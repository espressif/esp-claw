/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

#include "demos/lv_demos.h"

typedef void (*lua_lvgl_demo_entry_fn)(void);

typedef struct {
    const char *name;
    lua_lvgl_demo_entry_fn entry;
} lua_lvgl_demo_entry_t;

static const lua_lvgl_demo_entry_t s_lua_lvgl_demo_entries[] = {
#if LV_USE_DEMO_WIDGETS
    {"widgets", lv_demo_widgets},
#endif
#if LV_USE_DEMO_MUSIC
    {"music", lv_demo_music},
#endif
#if LV_USE_DEMO_STRESS
    {"stress", lv_demo_stress},
#endif
#if LV_USE_DEMO_KEYPAD_AND_ENCODER
    {"keypad_encoder", lv_demo_keypad_encoder},
#endif
#if LV_USE_DEMO_BENCHMARK
    {"benchmark", lv_demo_benchmark},
#endif
#if LV_USE_DEMO_VECTOR_GRAPHIC && LV_USE_VECTOR_GRAPHIC
    {"vector_graphic_buffered", lv_demo_vector_graphic_buffered},
    {"vector_graphic_not_buffered", lv_demo_vector_graphic_not_buffered},
#endif
    {NULL, NULL},
};

static const lua_lvgl_demo_entry_t *lua_lvgl_find_demo(const char *name)
{
    const lua_lvgl_demo_entry_t *entry = s_lua_lvgl_demo_entries;

    for (; entry->name; entry++) {
        if (strcmp(name, entry->name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static int lua_lvgl_demos(lua_State *L)
{
    const lua_lvgl_demo_entry_t *entry = s_lua_lvgl_demo_entries;
    int index = 1;

    lua_newtable(L);
    for (; entry->name; entry++) {
        lua_pushstring(L, entry->name);
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

static int lua_lvgl_demo(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const lua_lvgl_demo_entry_t *entry = lua_lvgl_find_demo(name);
    esp_err_t err;

    if (!entry || !entry->entry) {
        return luaL_error(L, "lvgl demo unavailable: %s", name);
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }

    entry->entry();

    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

const luaL_Reg lua_lvgl_demo_module_funcs[] = {
    {"demos", lua_lvgl_demos},
    {"demo", lua_lvgl_demo},
    {NULL, NULL},
};
