/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_ledc.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define LUA_MODULE_LEDC_METATABLE "ledc"
#define LUA_MODULE_LEDC_DEFAULT_DUTY_PERCENT 50.0
#define LUA_MODULE_LEDC_DEFAULT_DUTY_RES_BITS 14
#define LUA_MODULE_LEDC_MIN_DUTY_RES_BITS 1
#define LUA_MODULE_LEDC_MAX_DUTY_RES_BITS 14

#if CONFIG_ESP_BOARD_DEV_CAMERA_SUPPORT
/*
 * Some board-manager camera devices generate sensor XCLK with LEDC timer0 /
 * channel0 before Lua starts. The Lua-side allocator cannot see that external
 * ownership, so keep dynamic Lua PWM handles away from slot 0 on camera boards.
 */
#define LUA_MODULE_LEDC_FIRST_DYNAMIC_CHANNEL 1
#define LUA_MODULE_LEDC_FIRST_DYNAMIC_TIMER 1
#else
#define LUA_MODULE_LEDC_FIRST_DYNAMIC_CHANNEL 0
#define LUA_MODULE_LEDC_FIRST_DYNAMIC_TIMER 0
#endif

static const char *TAG = "lua_module_ledc";
static portMUX_TYPE s_ledc_alloc_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    bool in_use;
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    ledc_timer_bit_t duty_resolution;
    uint32_t frequency_hz;
    uint32_t refcount;
} lua_module_ledc_timer_slot_t;

typedef struct {
    int gpio;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_num;
    ledc_timer_bit_t duty_resolution;
    uint32_t frequency_hz;
    double duty_percent;
    bool running;
    bool allocated;
} lua_module_ledc_ud_t;

static lua_module_ledc_timer_slot_t s_timer_slots[LEDC_TIMER_MAX];
static bool s_channel_in_use[LEDC_CHANNEL_MAX];

static lua_module_ledc_ud_t *lua_module_ledc_get_ud(lua_State *L, int idx)
{
    lua_module_ledc_ud_t *ud =
        (lua_module_ledc_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_LEDC_METATABLE);
    if (!ud || !ud->allocated) {
        luaL_error(L, "ledc: invalid or closed handle");
    }
    return ud;
}

static uint32_t lua_module_ledc_calc_raw_duty(ledc_timer_bit_t duty_resolution,
                                              double duty_percent)
{
    const uint32_t max_duty = (1U << (uint32_t)duty_resolution) - 1U;
    double clamped = duty_percent;

    if (clamped < 0.0) {
        clamped = 0.0;
    } else if (clamped > 100.0) {
        clamped = 100.0;
    }

    return (uint32_t)(((double)max_duty * clamped / 100.0) + 0.5);
}

static bool lua_module_ledc_timer_slot_matches(const lua_module_ledc_timer_slot_t *slot,
                                               ledc_mode_t speed_mode,
                                               ledc_timer_bit_t duty_resolution,
                                               uint32_t frequency_hz)
{
    return slot->in_use &&
           slot->speed_mode == speed_mode &&
           slot->duty_resolution == duty_resolution &&
           slot->frequency_hz == frequency_hz;
}

static esp_err_t lua_module_ledc_alloc_channel(ledc_channel_t *out_channel)
{
    int selected = -1;

    portENTER_CRITICAL(&s_ledc_alloc_mux);
    for (int i = LUA_MODULE_LEDC_FIRST_DYNAMIC_CHANNEL; i < LEDC_CHANNEL_MAX; ++i) {
        if (!s_channel_in_use[i]) {
            s_channel_in_use[i] = true;
            selected = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_ledc_alloc_mux);

    if (selected < 0) {
        return ESP_ERR_NO_MEM;
    }

    *out_channel = (ledc_channel_t)selected;
    return ESP_OK;
}

static void lua_module_ledc_release_channel(ledc_channel_t channel)
{
    if ((int)channel < 0 || (int)channel >= LEDC_CHANNEL_MAX) {
        return;
    }

    portENTER_CRITICAL(&s_ledc_alloc_mux);
    s_channel_in_use[(int)channel] = false;
    portEXIT_CRITICAL(&s_ledc_alloc_mux);
}

static esp_err_t lua_module_ledc_acquire_timer(ledc_mode_t speed_mode,
                                               ledc_timer_bit_t duty_resolution,
                                               uint32_t frequency_hz,
                                               ledc_timer_t *out_timer)
{
    int slot_index = -1;
    bool needs_config = false;

    portENTER_CRITICAL(&s_ledc_alloc_mux);
    for (int i = LUA_MODULE_LEDC_FIRST_DYNAMIC_TIMER; i < LEDC_TIMER_MAX; ++i) {
        if (lua_module_ledc_timer_slot_matches(&s_timer_slots[i],
                                               speed_mode,
                                               duty_resolution,
                                               frequency_hz)) {
            s_timer_slots[i].refcount++;
            slot_index = i;
            break;
        }
    }

    if (slot_index < 0) {
        for (int i = LUA_MODULE_LEDC_FIRST_DYNAMIC_TIMER; i < LEDC_TIMER_MAX; ++i) {
            if (!s_timer_slots[i].in_use) {
                s_timer_slots[i] = (lua_module_ledc_timer_slot_t) {
                    .in_use = true,
                    .speed_mode = speed_mode,
                    .timer_num = (ledc_timer_t)i,
                    .duty_resolution = duty_resolution,
                    .frequency_hz = frequency_hz,
                    .refcount = 1,
                };
                slot_index = i;
                needs_config = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_ledc_alloc_mux);

    if (slot_index < 0) {
        return ESP_ERR_NO_MEM;
    }

    if (needs_config) {
        ledc_timer_config_t timer_config = {
            .speed_mode = speed_mode,
            .duty_resolution = duty_resolution,
            .timer_num = (ledc_timer_t)slot_index,
            .freq_hz = frequency_hz,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        esp_err_t err = ledc_timer_config(&timer_config);
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_ledc_alloc_mux);
            memset(&s_timer_slots[slot_index], 0, sizeof(s_timer_slots[slot_index]));
            portEXIT_CRITICAL(&s_ledc_alloc_mux);
            return err;
        }
    }

    *out_timer = (ledc_timer_t)slot_index;
    return ESP_OK;
}

static uint32_t lua_module_ledc_timer_refcount(ledc_timer_t timer_num)
{
    uint32_t refcount = 0;

    if ((int)timer_num < 0 || (int)timer_num >= LEDC_TIMER_MAX) {
        return 0;
    }

    portENTER_CRITICAL(&s_ledc_alloc_mux);
    refcount = s_timer_slots[(int)timer_num].refcount;
    portEXIT_CRITICAL(&s_ledc_alloc_mux);
    return refcount;
}

static void lua_module_ledc_update_timer_frequency(ledc_timer_t timer_num, uint32_t frequency_hz)
{
    if ((int)timer_num < 0 || (int)timer_num >= LEDC_TIMER_MAX) {
        return;
    }

    portENTER_CRITICAL(&s_ledc_alloc_mux);
    if (s_timer_slots[(int)timer_num].in_use) {
        s_timer_slots[(int)timer_num].frequency_hz = frequency_hz;
    }
    portEXIT_CRITICAL(&s_ledc_alloc_mux);
}

static void lua_module_ledc_release_timer(ledc_timer_t timer_num)
{
    if ((int)timer_num < 0 || (int)timer_num >= LEDC_TIMER_MAX) {
        return;
    }

    portENTER_CRITICAL(&s_ledc_alloc_mux);
    if (s_timer_slots[(int)timer_num].in_use) {
        if (s_timer_slots[(int)timer_num].refcount > 0) {
            s_timer_slots[(int)timer_num].refcount--;
        }
        if (s_timer_slots[(int)timer_num].refcount == 0) {
            memset(&s_timer_slots[(int)timer_num], 0, sizeof(s_timer_slots[(int)timer_num]));
        }
    }
    portEXIT_CRITICAL(&s_ledc_alloc_mux);
}

static esp_err_t lua_module_ledc_apply_duty(lua_module_ledc_ud_t *ud)
{
    const uint32_t raw_duty = lua_module_ledc_calc_raw_duty(ud->duty_resolution, ud->duty_percent);

    ESP_RETURN_ON_ERROR(
        ledc_set_duty(ud->speed_mode, ud->channel, raw_duty),
        TAG,
        "ledc_set_duty failed");
    ESP_RETURN_ON_ERROR(
        ledc_update_duty(ud->speed_mode, ud->channel),
        TAG,
        "ledc_update_duty failed");
    return ESP_OK;
}

static esp_err_t lua_module_ledc_destroy(lua_module_ledc_ud_t *ud)
{
    esp_err_t stop_err = ESP_OK;

    if (!ud || !ud->allocated) {
        return ESP_OK;
    }

    if (ud->running) {
        stop_err = ledc_stop(ud->speed_mode, ud->channel, 0);
        ud->running = false;
    }

    lua_module_ledc_release_channel(ud->channel);
    lua_module_ledc_release_timer(ud->timer_num);
    ud->allocated = false;
    ud->gpio = -1;

    return stop_err;
}

static void lua_module_ledc_parse_config(lua_State *L, int idx, lua_module_ledc_ud_t *config)
{
    lua_Integer duty_resolution_bits = LUA_MODULE_LEDC_DEFAULT_DUTY_RES_BITS;
    lua_Integer frequency_hz;

    luaL_checktype(L, idx, LUA_TTABLE);

    *config = (lua_module_ledc_ud_t) {
        .gpio = -1,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)LUA_MODULE_LEDC_DEFAULT_DUTY_RES_BITS,
        .duty_percent = LUA_MODULE_LEDC_DEFAULT_DUTY_PERCENT,
    };

    lua_getfield(L, idx, "gpio");
    if (lua_isnil(L, -1)) {
        luaL_error(L, "ledc.new: gpio is required");
    }
    config->gpio = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "frequency_hz");
    if (lua_isnil(L, -1)) {
        luaL_error(L, "ledc.new: frequency_hz is required");
    }
    frequency_hz = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "duty_percent");
    if (!lua_isnil(L, -1)) {
        config->duty_percent = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "duty_resolution_bits");
    if (!lua_isnil(L, -1)) {
        duty_resolution_bits = luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    if (config->gpio < 0) {
        luaL_error(L, "ledc.new: gpio must be >= 0");
    }
    if (frequency_hz <= 0 || (uint64_t)frequency_hz > UINT32_MAX) {
        luaL_error(L, "ledc.new: frequency_hz must be > 0");
    }
    config->frequency_hz = (uint32_t)frequency_hz;
    if (config->duty_percent < 0.0 || config->duty_percent > 100.0) {
        luaL_error(L, "ledc.new: duty_percent must be between 0 and 100");
    }
    if (duty_resolution_bits < LUA_MODULE_LEDC_MIN_DUTY_RES_BITS ||
            duty_resolution_bits > LUA_MODULE_LEDC_MAX_DUTY_RES_BITS) {
        luaL_error(L,
                   "ledc.new: duty_resolution_bits must be between %d and %d",
                   LUA_MODULE_LEDC_MIN_DUTY_RES_BITS,
                   LUA_MODULE_LEDC_MAX_DUTY_RES_BITS);
    }

    config->duty_resolution = (ledc_timer_bit_t)duty_resolution_bits;
}

static esp_err_t lua_module_ledc_create(lua_module_ledc_ud_t *ud)
{
    esp_err_t err = lua_module_ledc_alloc_channel(&ud->channel);
    if (err != ESP_OK) {
        return err;
    }

    err = lua_module_ledc_acquire_timer(ud->speed_mode,
                                        ud->duty_resolution,
                                        ud->frequency_hz,
                                        &ud->timer_num);
    if (err != ESP_OK) {
        lua_module_ledc_release_channel(ud->channel);
        return err;
    }

    ledc_channel_config_t channel_config = {
        .gpio_num = ud->gpio,
        .speed_mode = ud->speed_mode,
        .channel = ud->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = ud->timer_num,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel_config);
    if (err != ESP_OK) {
        lua_module_ledc_release_channel(ud->channel);
        lua_module_ledc_release_timer(ud->timer_num);
        return err;
    }

    ud->allocated = true;
    ud->running = false;
    return ESP_OK;
}

static int lua_module_ledc_gc(lua_State *L)
{
    lua_module_ledc_ud_t *ud =
        (lua_module_ledc_ud_t *)luaL_testudata(L, 1, LUA_MODULE_LEDC_METATABLE);
    if (ud) {
        (void)lua_module_ledc_destroy(ud);
    }
    return 0;
}

static int lua_module_ledc_start(lua_State *L)
{
    lua_module_ledc_ud_t *ud = lua_module_ledc_get_ud(L, 1);
    esp_err_t err;

    if (!ud->running) {
        err = lua_module_ledc_apply_duty(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc start failed: %s", esp_err_to_name(err));
        }
        ud->running = true;
    }
    return 0;
}

static int lua_module_ledc_stop(lua_State *L)
{
    lua_module_ledc_ud_t *ud = lua_module_ledc_get_ud(L, 1);
    if (ud->running) {
        esp_err_t err = ledc_stop(ud->speed_mode, ud->channel, 0);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc stop failed: %s", esp_err_to_name(err));
        }
        ud->running = false;
    }
    return 0;
}

static int lua_module_ledc_set_duty(lua_State *L)
{
    lua_module_ledc_ud_t *ud = lua_module_ledc_get_ud(L, 1);
    const double duty_percent = luaL_checknumber(L, 2);

    if (duty_percent < 0.0 || duty_percent > 100.0) {
        return luaL_error(L, "ledc duty_percent must be between 0 and 100");
    }

    ud->duty_percent = duty_percent;
    if (ud->running) {
        esp_err_t err = lua_module_ledc_apply_duty(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc set_duty failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_module_ledc_set_frequency(lua_State *L)
{
    lua_module_ledc_ud_t *ud = lua_module_ledc_get_ud(L, 1);
    const uint32_t frequency_hz = (uint32_t)luaL_checkinteger(L, 2);
    const uint32_t refcount = lua_module_ledc_timer_refcount(ud->timer_num);
    uint32_t actual_frequency = 0;
    esp_err_t err;

    if (frequency_hz == 0) {
        return luaL_error(L, "ledc frequency_hz must be > 0");
    }
    if (frequency_hz == ud->frequency_hz) {
        return 0;
    }
    if (refcount > 1) {
        return luaL_error(L,
                          "ledc set_frequency cannot reconfigure a shared timer; close other handles first");
    }

    actual_frequency = ledc_set_freq(ud->speed_mode, ud->timer_num, frequency_hz);
    if (actual_frequency == 0) {
        return luaL_error(L, "ledc set_frequency failed");
    }

    ud->frequency_hz = actual_frequency;
    lua_module_ledc_update_timer_frequency(ud->timer_num, actual_frequency);

    if (ud->running) {
        err = lua_module_ledc_apply_duty(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc set_frequency failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_module_ledc_close(lua_State *L)
{
    lua_module_ledc_ud_t *ud =
        (lua_module_ledc_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_LEDC_METATABLE);
    esp_err_t err = lua_module_ledc_destroy(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "ledc close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_ledc_new(lua_State *L)
{
    lua_module_ledc_ud_t config = {0};
    lua_module_ledc_parse_config(L, 1, &config);

    lua_module_ledc_ud_t *ud =
        (lua_module_ledc_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = config;

    esp_err_t err = lua_module_ledc_create(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "ledc new failed: %s", esp_err_to_name(err));
    }

    luaL_getmetatable(L, LUA_MODULE_LEDC_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_ledc(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_LEDC_METATABLE)) {
        lua_pushcfunction(L, lua_module_ledc_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_ledc_start);
        lua_setfield(L, -2, "start");
        lua_pushcfunction(L, lua_module_ledc_stop);
        lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, lua_module_ledc_set_duty);
        lua_setfield(L, -2, "set_duty");
        lua_pushcfunction(L, lua_module_ledc_set_frequency);
        lua_setfield(L, -2, "set_frequency");
        lua_pushcfunction(L, lua_module_ledc_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_ledc_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_ledc_register(void)
{
    return cap_lua_register_module("ledc", luaopen_ledc);
}
