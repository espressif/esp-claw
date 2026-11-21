/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_ledc.h"

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
#include "freertos/semphr.h"
#include "cap_lua_hw.h"
#include "driver/gpio.h"

#define LUA_DRIVER_LEDC_METATABLE "ledc"
#define LUA_DRIVER_LEDC_DEFAULT_DUTY_PERCENT 50.0
#define LUA_DRIVER_LEDC_DEFAULT_DUTY_RES_BITS 14
#define LUA_DRIVER_LEDC_MIN_DUTY_RES_BITS 1
#define LUA_DRIVER_LEDC_MAX_DUTY_RES_BITS 14

static const char *TAG = "lua_driver_ledc";
static SemaphoreHandle_t s_ledc_lock;

typedef struct {
    bool in_use;
    bool ready;
    cap_lua_hw_lease_t lease;
    ledc_mode_t speed_mode;
    ledc_timer_t timer_num;
    ledc_timer_bit_t duty_resolution;
    uint32_t frequency_hz;
    uint32_t refcount;
} lua_driver_ledc_timer_slot_t;

typedef struct {
    int gpio;
    cap_lua_hw_lease_t lease;
    ledc_mode_t speed_mode;
    ledc_channel_t channel;
    ledc_timer_t timer_num;
    ledc_timer_bit_t duty_resolution;
    uint32_t frequency_hz;
    double duty_percent;
    bool running;
    bool allocated;
    bool channel_configured;
    bool closing;
} lua_driver_ledc_ud_t;

static lua_driver_ledc_timer_slot_t s_timer_slots[LEDC_TIMER_MAX];


static lua_driver_ledc_ud_t *lua_driver_ledc_get_ud(lua_State *L, int idx)
{
    lua_driver_ledc_ud_t *ud =
        (lua_driver_ledc_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_LEDC_METATABLE);
    if (!ud || !ud->allocated || ud->closing) {
        luaL_error(L, "ledc: invalid or closed handle");
    }
    return ud;
}

static uint32_t lua_driver_ledc_calc_raw_duty(ledc_timer_bit_t duty_resolution,
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

static bool lua_driver_ledc_timer_slot_matches(const lua_driver_ledc_timer_slot_t *slot,
                                               ledc_mode_t speed_mode,
                                               ledc_timer_bit_t duty_resolution,
                                               uint32_t frequency_hz)
{
    return slot->in_use && slot->ready &&
           slot->speed_mode == speed_mode &&
           slot->duty_resolution == duty_resolution &&
           slot->frequency_hz == frequency_hz;
}

static esp_err_t lua_driver_ledc_acquire_timer(lua_State *L, lua_driver_ledc_ud_t *ud)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_ledc_lock, portMAX_DELAY);
    for (int i = 0; i < LEDC_TIMER_MAX; ++i) {
        lua_driver_ledc_timer_slot_t *slot = &s_timer_slots[i];
        if (lua_driver_ledc_timer_slot_matches(slot, ud->speed_mode, ud->duty_resolution, ud->frequency_hz) &&
            slot->lease.registry == cap_lua_hw_get_registry(L)) {
            slot->refcount++;
            ud->timer_num = i;
            xSemaphoreGive(s_ledc_lock);
            return ESP_OK;
        }
    }
    for (int i = 0; i < LEDC_TIMER_MAX; ++i) {
        lua_driver_ledc_timer_slot_t *slot = &s_timer_slots[i];
        if (slot->in_use) continue;
        char key[CLAW_HW_KEY_SIZE];
        const char *keys[] = { claw_hw_key_ledc_timer(key, sizeof(key), ud->speed_mode, i) };
        err = cap_lua_hw_claim(L, &slot->lease, "lua/ledc-timer", keys, 1);
        if (err != ESP_OK) continue;
        slot->in_use = true;
        slot->speed_mode = ud->speed_mode;
        slot->timer_num = i;
        slot->duty_resolution = ud->duty_resolution;
        slot->frequency_hz = ud->frequency_hz;
        slot->refcount = 1;
        ud->timer_num = i;
        ledc_timer_config_t cfg = {
            .speed_mode = ud->speed_mode, .duty_resolution = ud->duty_resolution,
            .timer_num = i, .freq_hz = ud->frequency_hz, .clk_cfg = LEDC_AUTO_CLK,
        };
        err = ledc_timer_config(&cfg);
        slot->ready = err == ESP_OK;
        if (err != ESP_OK) {
            /* Retain the timer until the object's close path confirms it stopped. */
            ud->allocated = true;
        }
        xSemaphoreGive(s_ledc_lock);
        return err;
    }
    xSemaphoreGive(s_ledc_lock);
    return err;
}

static esp_err_t lua_driver_ledc_release_timer(ledc_timer_t timer_num)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_ledc_lock, portMAX_DELAY);
    lua_driver_ledc_timer_slot_t *slot = &s_timer_slots[timer_num];
    if (slot->refcount == 1) {
        err = ledc_timer_pause(slot->speed_mode, timer_num);
        if (err == ESP_OK) err = cap_lua_hw_release(&slot->lease);
    }
    if (err == ESP_OK && --slot->refcount == 0) memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(s_ledc_lock);
    return err;
}

static esp_err_t lua_driver_ledc_apply_duty(lua_driver_ledc_ud_t *ud)
{
    const uint32_t raw_duty = lua_driver_ledc_calc_raw_duty(ud->duty_resolution, ud->duty_percent);

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

static esp_err_t lua_driver_ledc_destroy(lua_driver_ledc_ud_t *ud)
{
    if (!ud) return ESP_OK;
    ud->closing = true;
    if (ud->allocated) {
        esp_err_t err = ud->channel_configured ? ledc_stop(ud->speed_mode, ud->channel, 0) : ESP_OK;
        if (err != ESP_OK) return err;
        ud->channel_configured = false;
        ud->running = false;
        err = gpio_reset_pin(ud->gpio);
        if (err != ESP_OK) return err;
        err = lua_driver_ledc_release_timer(ud->timer_num);
        if (err != ESP_OK) return err;
        ud->allocated = false;
    }
    return cap_lua_hw_release(&ud->lease);
}

static void lua_driver_ledc_parse_config(lua_State *L, int idx, lua_driver_ledc_ud_t *config)
{
    lua_Integer duty_resolution_bits = LUA_DRIVER_LEDC_DEFAULT_DUTY_RES_BITS;
    lua_Integer frequency_hz;

    luaL_checktype(L, idx, LUA_TTABLE);

    *config = (lua_driver_ledc_ud_t) {
        .gpio = -1,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)LUA_DRIVER_LEDC_DEFAULT_DUTY_RES_BITS,
        .duty_percent = LUA_DRIVER_LEDC_DEFAULT_DUTY_PERCENT,
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
    if (duty_resolution_bits < LUA_DRIVER_LEDC_MIN_DUTY_RES_BITS ||
            duty_resolution_bits > LUA_DRIVER_LEDC_MAX_DUTY_RES_BITS) {
        luaL_error(L,
                   "ledc.new: duty_resolution_bits must be between %d and %d",
                   LUA_DRIVER_LEDC_MIN_DUTY_RES_BITS,
                   LUA_DRIVER_LEDC_MAX_DUTY_RES_BITS);
    }

    config->duty_resolution = (ledc_timer_bit_t)duty_resolution_bits;
}

static esp_err_t lua_driver_ledc_create(lua_State *L, lua_driver_ledc_ud_t *ud)
{
    char key_buf[2][32];
    const char *keys[] = { claw_hw_key_gpio(key_buf[0], sizeof(key_buf[0]), ud->gpio), NULL };
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < LEDC_CHANNEL_MAX; ++i) {
        keys[1] = claw_hw_key_ledc_channel(key_buf[1], sizeof(key_buf[1]), ud->speed_mode, i);
        err = cap_lua_hw_claim(L, &ud->lease, "lua/ledc-channel", keys, 2);
        if (err == ESP_OK) { ud->channel = i; break; }
    }
    if (err != ESP_OK) return err;
    err = lua_driver_ledc_acquire_timer(L, ud);
    if (err != ESP_OK) {
        lua_driver_ledc_destroy(ud);
        return err;
    }
    ud->allocated = true;
    ledc_channel_config_t cfg = {
        .gpio_num = ud->gpio, .speed_mode = ud->speed_mode, .channel = ud->channel,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = ud->timer_num, .duty = 0, .hpoint = 0,
    };
    ud->channel_configured = true;
    err = ledc_channel_config(&cfg);
    if (err != ESP_OK) lua_driver_ledc_destroy(ud);
    else ESP_LOGI(TAG, "GPIO%d mode=%d channel=%d timer=%d frequency=%lu resolution=%d", ud->gpio, ud->speed_mode, ud->channel, ud->timer_num, (unsigned long)ud->frequency_hz, ud->duty_resolution);
    return err;
}

static int lua_driver_ledc_gc(lua_State *L)
{
    lua_driver_ledc_ud_t *ud =
        (lua_driver_ledc_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_LEDC_METATABLE);
    if (ud) {
        (void)lua_driver_ledc_destroy(ud);
    }
    return 0;
}

static int lua_driver_ledc_start(lua_State *L)
{
    lua_driver_ledc_ud_t *ud = lua_driver_ledc_get_ud(L, 1);
    esp_err_t err;

    if (!ud->running) {
        err = lua_driver_ledc_apply_duty(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc start failed: %s", esp_err_to_name(err));
        }
        ud->running = true;
    }
    return 0;
}

static int lua_driver_ledc_stop(lua_State *L)
{
    lua_driver_ledc_ud_t *ud = lua_driver_ledc_get_ud(L, 1);
    if (ud->running) {
        esp_err_t err = ledc_stop(ud->speed_mode, ud->channel, 0);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc stop failed: %s", esp_err_to_name(err));
        }
        ud->running = false;
    }
    return 0;
}

static int lua_driver_ledc_set_duty(lua_State *L)
{
    lua_driver_ledc_ud_t *ud = lua_driver_ledc_get_ud(L, 1);
    const double duty_percent = luaL_checknumber(L, 2);

    if (duty_percent < 0.0 || duty_percent > 100.0) {
        return luaL_error(L, "ledc duty_percent must be between 0 and 100");
    }

    ud->duty_percent = duty_percent;
    if (ud->running) {
        esp_err_t err = lua_driver_ledc_apply_duty(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "ledc set_duty failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_driver_ledc_set_frequency(lua_State *L)
{
    lua_driver_ledc_ud_t *ud = lua_driver_ledc_get_ud(L, 1);
    lua_Integer frequency = luaL_checkinteger(L, 2);
    luaL_argcheck(L, frequency > 0 && (uint64_t)frequency <= UINT32_MAX, 2, "invalid frequency");
    xSemaphoreTake(s_ledc_lock, portMAX_DELAY);
    lua_driver_ledc_timer_slot_t *slot = &s_timer_slots[ud->timer_num];
    if (slot->refcount != 1) {
        xSemaphoreGive(s_ledc_lock);
        return luaL_error(L, "ledc timer is shared; cannot change its frequency");
    }
    esp_err_t err = ledc_set_freq(ud->speed_mode, ud->timer_num, frequency);
    if (err == ESP_OK) ud->frequency_hz = slot->frequency_hz = frequency;
    xSemaphoreGive(s_ledc_lock);
    if (err != ESP_OK) return luaL_error(L, "ledc set_frequency failed: %s", esp_err_to_name(err));
    return 0;
}

static int lua_driver_ledc_get_info(lua_State *L)
{
    lua_driver_ledc_ud_t *ud = lua_driver_ledc_get_ud(L, 1);
    xSemaphoreTake(s_ledc_lock, portMAX_DELAY);
    uint32_t frequency = ledc_get_freq(ud->speed_mode, ud->timer_num);
    uint32_t duty = ledc_get_duty(ud->speed_mode, ud->channel);
    uint32_t refcount = s_timer_slots[ud->timer_num].refcount;
    xSemaphoreGive(s_ledc_lock);
    if (frequency == 0 || duty == UINT32_MAX) {
        ESP_LOGE(TAG, "Failed to read LEDC timer=%d channel=%d", ud->timer_num, ud->channel);
        return luaL_error(L, "ledc get_info failed: cannot read hardware state");
    }

    /* Lua allocations may run finalizers that acquire the same lock. */
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, ud->gpio);
    lua_setfield(L, -2, "gpio");
    lua_pushinteger(L, ud->channel);
    lua_setfield(L, -2, "channel");
    lua_pushinteger(L, ud->timer_num);
    lua_setfield(L, -2, "timer");
    lua_pushinteger(L, frequency);
    lua_setfield(L, -2, "frequency");
    lua_pushinteger(L, ud->duty_resolution);
    lua_setfield(L, -2, "resolution");
    lua_pushinteger(L, duty);
    lua_setfield(L, -2, "duty");
    lua_pushinteger(L, refcount);
    lua_setfield(L, -2, "timer_ref_count");
    return 1;
}

static int lua_driver_ledc_close(lua_State *L)
{
    lua_driver_ledc_ud_t *ud =
        (lua_driver_ledc_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_LEDC_METATABLE);
    esp_err_t err = lua_driver_ledc_destroy(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "ledc close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_ledc_new(lua_State *L)
{
    lua_driver_ledc_ud_t config = {0};
    lua_driver_ledc_parse_config(L, 1, &config);

    lua_driver_ledc_ud_t *ud =
        (lua_driver_ledc_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = config;
    luaL_getmetatable(L, LUA_DRIVER_LEDC_METATABLE);
    lua_setmetatable(L, -2);

    esp_err_t err = lua_driver_ledc_create(L, ud);
    if (err != ESP_OK) {
        (void)lua_driver_ledc_destroy(ud);
        return luaL_error(L, "ledc new failed: %s", esp_err_to_name(err));
    }

    return 1;
}

int luaopen_ledc(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_LEDC_METATABLE)) {
        lua_pushcfunction(L, lua_driver_ledc_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_ledc_start);
        lua_setfield(L, -2, "start");
        lua_pushcfunction(L, lua_driver_ledc_stop);
        lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, lua_driver_ledc_set_duty);
        lua_setfield(L, -2, "set_duty");
        lua_pushcfunction(L, lua_driver_ledc_set_frequency);
        lua_setfield(L, -2, "set_frequency");
        lua_pushcfunction(L, lua_driver_ledc_get_info);
        lua_setfield(L, -2, "get_info");
        lua_pushcfunction(L, lua_driver_ledc_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_ledc_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_ledc_register(void)
{
    if (!s_ledc_lock) s_ledc_lock = xSemaphoreCreateMutex();
    if (!s_ledc_lock) return ESP_ERR_NO_MEM;
    return cap_lua_register_module("ledc", luaopen_ledc);
}
