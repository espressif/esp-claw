/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_adc.h"

#include <stdbool.h>

#include "cap_lua.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/adc_types.h"
#include "lauxlib.h"

#define LUA_DRIVER_ADC_METATABLE "adc.channel"
#define LUA_DRIVER_ADC_DEFAULT_ATTEN ADC_ATTEN_DB_12

typedef enum {
    LUA_DRIVER_ADC_CALI_NONE = 0,
    LUA_DRIVER_ADC_CALI_CURVE,
    LUA_DRIVER_ADC_CALI_LINE,
} lua_driver_adc_cali_scheme_t;

typedef struct {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    lua_driver_adc_cali_scheme_t cali_scheme;
    adc_unit_t unit_id;
    adc_channel_t channel;
    int gpio_num;
} lua_driver_adc_ud_t;

static adc_oneshot_unit_handle_t s_units[SOC_ADC_PERIPH_NUM];
static int s_unit_refcount[SOC_ADC_PERIPH_NUM];
static SemaphoreHandle_t s_units_lock;

static esp_err_t lua_driver_adc_acquire_unit(adc_unit_t unit_id,
                                             adc_oneshot_unit_handle_t *out_handle)
{
    if (unit_id >= SOC_ADC_PERIPH_NUM) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_units_lock, portMAX_DELAY);
    if (s_units[unit_id] == NULL) {
        adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id = unit_id,
        };
        err = adc_oneshot_new_unit(&cfg, &s_units[unit_id]);
        if (err != ESP_OK) {
            s_units[unit_id] = NULL;
            xSemaphoreGive(s_units_lock);
            return err;
        }
    }
    s_unit_refcount[unit_id]++;
    *out_handle = s_units[unit_id];
    xSemaphoreGive(s_units_lock);
    return ESP_OK;
}

static void lua_driver_adc_release_unit(adc_unit_t unit_id)
{
    if (unit_id >= SOC_ADC_PERIPH_NUM) {
        return;
    }
    xSemaphoreTake(s_units_lock, portMAX_DELAY);
    if (s_units[unit_id] != NULL && s_unit_refcount[unit_id] > 0) {
        if (--s_unit_refcount[unit_id] == 0) {
            adc_oneshot_del_unit(s_units[unit_id]);
            s_units[unit_id] = NULL;
        }
    }
    xSemaphoreGive(s_units_lock);
}

static esp_err_t lua_driver_adc_try_calibrate(lua_driver_adc_ud_t *ud)
{
    ud->cali = NULL;
    ud->cali_scheme = LUA_DRIVER_ADC_CALI_NONE;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cfg = {
            .unit_id = ud->unit_id,
            .chan = ud->channel,
            .atten = LUA_DRIVER_ADC_DEFAULT_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cfg, &ud->cali) == ESP_OK) {
            ud->cali_scheme = LUA_DRIVER_ADC_CALI_CURVE;
            return ESP_OK;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    {
        adc_cali_line_fitting_config_t cfg = {
            .unit_id = ud->unit_id,
            .atten = LUA_DRIVER_ADC_DEFAULT_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&cfg, &ud->cali) == ESP_OK) {
            ud->cali_scheme = LUA_DRIVER_ADC_CALI_LINE;
            return ESP_OK;
        }
    }
#endif

    return ESP_ERR_NOT_SUPPORTED;
}

static void lua_driver_adc_release_cali(lua_driver_adc_ud_t *ud)
{
    if (ud->cali == NULL) {
        return;
    }
    switch (ud->cali_scheme) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    case LUA_DRIVER_ADC_CALI_CURVE:
        adc_cali_delete_scheme_curve_fitting(ud->cali);
        break;
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    case LUA_DRIVER_ADC_CALI_LINE:
        adc_cali_delete_scheme_line_fitting(ud->cali);
        break;
#endif
    default:
        break;
    }
    ud->cali = NULL;
    ud->cali_scheme = LUA_DRIVER_ADC_CALI_NONE;
}

static lua_driver_adc_ud_t *lua_driver_adc_get_ud(lua_State *L, int idx)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_ADC_METATABLE);
    if (!ud || !ud->unit) {
        luaL_error(L, "adc channel: invalid or closed handle");
    }
    return ud;
}

static int lua_driver_adc_new(lua_State *L)
{
    int gpio = (int)luaL_checkinteger(L, 1);

    adc_unit_t unit_id;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit_id, &channel);
    if (err != ESP_OK) {
        return luaL_error(L, "GPIO %d is not a valid ADC pin: %s",
                          gpio, esp_err_to_name(err));
    }

    adc_oneshot_unit_handle_t unit = NULL;
    err = lua_driver_adc_acquire_unit(unit_id, &unit);
    if (err != ESP_OK || !unit) {
        return luaL_error(L, "adc unit %d init failed: %s",
                          (int)unit_id + 1, esp_err_to_name(err));
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = LUA_DRIVER_ADC_DEFAULT_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(unit, channel, &chan_cfg);
    if (err != ESP_OK) {
        lua_driver_adc_release_unit(unit_id);
        return luaL_error(L, "adc config channel failed: %s", esp_err_to_name(err));
    }

    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->unit = unit;
    ud->cali = NULL;
    ud->cali_scheme = LUA_DRIVER_ADC_CALI_NONE;
    ud->unit_id = unit_id;
    ud->channel = channel;
    ud->gpio_num = gpio;
    luaL_getmetatable(L, LUA_DRIVER_ADC_METATABLE);
    lua_setmetatable(L, -2);

    err = lua_driver_adc_try_calibrate(ud);
    if (err != ESP_OK) {
        return luaL_error(L,
                          "adc calibration unavailable on this chip (eFuse not "
                          "burnt or scheme unsupported)");
    }

    return 1;
}

static int lua_driver_adc_read(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    int raw = 0;
    esp_err_t err = adc_oneshot_read(ud->unit, ud->channel, &raw);
    if (err != ESP_OK) {
        return luaL_error(L, "adc read failed: %s", esp_err_to_name(err));
    }
    int mv = 0;
    err = adc_cali_raw_to_voltage(ud->cali, raw, &mv);
    if (err != ESP_OK) {
        return luaL_error(L, "adc cali failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, mv);
    return 1;
}

static int lua_driver_adc_get_gpio(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    lua_pushinteger(L, ud->gpio_num);
    return 1;
}

static int lua_driver_adc_gc(lua_State *L)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_ADC_METATABLE);
    if (ud) {
        lua_driver_adc_release_cali(ud);
        if (ud->unit) {
            lua_driver_adc_release_unit(ud->unit_id);
            ud->unit = NULL;
        }
    }
    return 0;
}

static int lua_driver_adc_close(lua_State *L)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_ADC_METATABLE);
    lua_driver_adc_release_cali(ud);
    if (ud->unit) {
        lua_driver_adc_release_unit(ud->unit_id);
        ud->unit = NULL;
    }
    return 0;
}

int luaopen_adc(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_ADC_METATABLE)) {
        lua_pushcfunction(L, lua_driver_adc_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_adc_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_adc_get_gpio);
        lua_setfield(L, -2, "get_gpio");
        lua_pushcfunction(L, lua_driver_adc_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_adc_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_adc_register(void)
{
    if (!s_units_lock) {
        s_units_lock = xSemaphoreCreateMutex();
        if (!s_units_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    return cap_lua_register_module("adc", luaopen_adc);
}
