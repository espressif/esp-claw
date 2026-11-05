/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_adc.h"

#include <stdbool.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/adc_types.h"
#include "lauxlib.h"
#include "cap_lua_hw.h"
#include "soc/soc_caps.h"

#define LUA_DRIVER_ADC_METATABLE "adc.channel"
#define LUA_DRIVER_ADC_DEFAULT_ATTEN ADC_ATTEN_DB_0
#define LUA_DRIVER_ADC_DEFAULT_BITWIDTH ADC_BITWIDTH_DEFAULT

#ifndef SOC_ADC_RTC_MAX_BITWIDTH
#define SOC_ADC_RTC_MAX_BITWIDTH SOC_ADC_DIGI_MAX_BITWIDTH
#endif
#ifndef SOC_ADC_RTC_MIN_BITWIDTH
#define SOC_ADC_RTC_MIN_BITWIDTH SOC_ADC_DIGI_MIN_BITWIDTH
#endif

static const char *TAG = "lua_adc";

typedef struct {
    cap_lua_hw_lease_t lease;
    bool closing;
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali;
    esp_err_t cali_err;
    adc_unit_t unit_id;
    adc_channel_t channel;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
    int gpio_num;
} lua_driver_adc_ud_t;

static adc_oneshot_unit_handle_t s_units[SOC_ADC_PERIPH_NUM];
static cap_lua_hw_lease_t s_unit_leases[SOC_ADC_PERIPH_NUM];
static int s_unit_refcount[SOC_ADC_PERIPH_NUM];
static bool s_active_gpios[SOC_GPIO_PIN_COUNT];
static SemaphoreHandle_t s_units_lock;

static esp_err_t lua_driver_adc_acquire_channel(lua_State *L, int gpio, adc_unit_t unit_id, adc_channel_t channel, adc_atten_t atten,
                                                adc_bitwidth_t bitwidth, adc_oneshot_unit_handle_t *out_unit)
{
    if ((unsigned)gpio >= SOC_GPIO_PIN_COUNT || (unsigned)unit_id >= SOC_ADC_PERIPH_NUM) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_units_lock, portMAX_DELAY);
    if (s_active_gpios[gpio]) {
        xSemaphoreGive(s_units_lock);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    bool unit_created = false;
    if (s_units[unit_id] != NULL && s_unit_leases[unit_id].registry != cap_lua_hw_get_registry(L)) {
        xSemaphoreGive(s_units_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_units[unit_id] == NULL) {
        char key[CLAW_HW_KEY_SIZE];
        const char *keys[] = { claw_hw_key_adc_unit(key, sizeof(key), unit_id) };
        err = cap_lua_hw_claim(L, &s_unit_leases[unit_id], "lua/adc-unit", keys, 1);
        if (err != ESP_OK) {
            xSemaphoreGive(s_units_lock);
            return err;
        }
        adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id = unit_id,
        };
        err = adc_oneshot_new_unit(&cfg, &s_units[unit_id]);
        if (err != ESP_OK) {
            s_units[unit_id] = NULL;
            cap_lua_hw_release(&s_unit_leases[unit_id]);
            xSemaphoreGive(s_units_lock);
            return err;
        }
        unit_created = true;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = atten,
        .bitwidth = bitwidth,
    };
    err = adc_oneshot_config_channel(s_units[unit_id], channel, &chan_cfg);
    if (err != ESP_OK) {
        if (unit_created) {
            esp_err_t delete_err = adc_oneshot_del_unit(s_units[unit_id]);
            if (delete_err == ESP_OK) {
                s_units[unit_id] = NULL;
                cap_lua_hw_release(&s_unit_leases[unit_id]);
            } else {
                s_active_gpios[gpio] = true;
                s_unit_refcount[unit_id]++;
                *out_unit = s_units[unit_id];
                ESP_LOGE(TAG, "Failed to roll back ADC unit %d: %s", (int)unit_id + 1, esp_err_to_name(delete_err));
            }
        }
        xSemaphoreGive(s_units_lock);
        return err;
    }

    s_active_gpios[gpio] = true;
    s_unit_refcount[unit_id]++;
    *out_unit = s_units[unit_id];
    xSemaphoreGive(s_units_lock);
    return ESP_OK;
}

static esp_err_t lua_driver_adc_release_channel(int gpio, adc_unit_t unit_id)
{
    xSemaphoreTake(s_units_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_active_gpios[gpio]) {
        if (s_unit_refcount[unit_id] == 1) {
            err = s_units[unit_id] ? adc_oneshot_del_unit(s_units[unit_id]) : ESP_OK;
            if (err == ESP_OK) {
                s_units[unit_id] = NULL;
                err = cap_lua_hw_release(&s_unit_leases[unit_id]);
            }
        }
        if (err == ESP_OK) {
            s_active_gpios[gpio] = false;
            s_unit_refcount[unit_id]--;
        }
    }
    xSemaphoreGive(s_units_lock);
    return err;
}

static esp_err_t lua_driver_adc_try_calibrate(lua_driver_adc_ud_t *ud)
{
    ud->cali = NULL;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = ud->unit_id,
        .chan = ud->channel,
        .atten = ud->atten,
        .bitwidth = ud->bitwidth,
    };
    return adc_cali_create_scheme_curve_fitting(&cfg, &ud->cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = ud->unit_id,
        .atten = ud->atten,
        .bitwidth = ud->bitwidth,
    };
    return adc_cali_create_scheme_line_fitting(&cfg, &ud->cali);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t lua_driver_adc_release_cali(lua_driver_adc_ud_t *ud)
{
    if (ud->cali == NULL) {
        return ESP_OK;
    }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    esp_err_t err = adc_cali_delete_scheme_curve_fitting(ud->cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    esp_err_t err = adc_cali_delete_scheme_line_fitting(ud->cali);
#endif
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED || ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to delete ADC calibration for GPIO %d: %s", ud->gpio_num, esp_err_to_name(err));
        return err;
    }
#endif
    ud->cali = NULL;
    return ESP_OK;
}

static lua_driver_adc_ud_t *lua_driver_adc_get_ud(lua_State *L, int idx)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_ADC_METATABLE);
    if (!ud || !ud->unit || ud->closing) {
        luaL_error(L, "adc channel: invalid or closed handle");
    }
    return ud;
}

static lua_Integer lua_driver_adc_get_integer_field(lua_State *L, int index, const char *name, lua_Integer default_value)
{
    lua_pushstring(L, name);
    lua_rawget(L, index);
    lua_Integer value = lua_isnil(L, -1) ? default_value : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static void lua_driver_adc_validate_config(lua_State *L, int index)
{
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            luaL_error(L, "adc config keys must be strings");
        }
        const char *name = lua_tostring(L, -2);
        if (strcmp(name, "atten") != 0 && strcmp(name, "bitwidth") != 0) {
            luaL_error(L, "unknown adc config field: %s", name);
        }
        lua_pop(L, 1);
    }
}

static void lua_driver_adc_parse_config(lua_State *L, adc_atten_t *atten, adc_bitwidth_t *bitwidth)
{
    lua_Integer atten_value = LUA_DRIVER_ADC_DEFAULT_ATTEN;
    lua_Integer bitwidth_value = LUA_DRIVER_ADC_DEFAULT_BITWIDTH;
    luaL_argcheck(L, lua_gettop(L) <= 2, 3, "unexpected argument");
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_driver_adc_validate_config(L, 2);
        atten_value = lua_driver_adc_get_integer_field(L, 2, "atten", atten_value);
        bitwidth_value = lua_driver_adc_get_integer_field(L, 2, "bitwidth", bitwidth_value);
    }

    luaL_argcheck(L, atten_value >= ADC_ATTEN_DB_0 && atten_value < SOC_ADC_ATTEN_NUM, 2, "atten unsupported on this chip");
    luaL_argcheck(L, bitwidth_value == ADC_BITWIDTH_DEFAULT ||
                         (bitwidth_value >= SOC_ADC_RTC_MIN_BITWIDTH && bitwidth_value <= SOC_ADC_RTC_MAX_BITWIDTH),
                  2, "bitwidth unsupported on this chip");
    *atten = (adc_atten_t)atten_value;
    *bitwidth = (adc_bitwidth_t)bitwidth_value;
}

static esp_err_t lua_driver_adc_close_ud(lua_driver_adc_ud_t *ud);

static int lua_driver_adc_new(lua_State *L)
{
    lua_Integer gpio_value = luaL_checkinteger(L, 1);
    luaL_argcheck(L, gpio_value >= 0 && gpio_value < SOC_GPIO_PIN_COUNT, 1, "GPIO out of range");
    int gpio = (int)gpio_value;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
    lua_driver_adc_parse_config(L, &atten, &bitwidth);

    adc_unit_t unit_id;
    adc_channel_t channel;
    esp_err_t err = adc_oneshot_io_to_channel(gpio, &unit_id, &channel);
    if (err != ESP_OK) {
        return luaL_error(L, "GPIO %d is not a valid ADC pin: %s",
                          gpio, esp_err_to_name(err));
    }

    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->unit_id = unit_id;
    ud->channel = channel;
    ud->atten = atten;
    ud->bitwidth = bitwidth;
    ud->gpio_num = gpio;
    luaL_getmetatable(L, LUA_DRIVER_ADC_METATABLE);
    lua_setmetatable(L, -2);

    char key_buf[2][32];
    const char *keys[] = {
        claw_hw_key_gpio(key_buf[0], sizeof(key_buf[0]), gpio),
        claw_hw_key_adc_channel(key_buf[1], sizeof(key_buf[1]), unit_id, channel),
    };
    err = cap_lua_hw_claim(L, &ud->lease, "lua/adc-channel", keys, 2);
    if (err != ESP_OK) return luaL_error(L, "adc claim failed: %s", esp_err_to_name(err));
    err = lua_driver_adc_acquire_channel(L, gpio, unit_id, channel, atten, bitwidth, &ud->unit);
    if (err != ESP_OK) {
        (void)lua_driver_adc_close_ud(ud);
        if (err == ESP_ERR_INVALID_STATE) {
            return luaL_error(L, "GPIO %d already has an active ADC channel", gpio);
        }
        return luaL_error(L, "adc channel init failed for GPIO %d: %s", gpio, esp_err_to_name(err));
    }

    ud->cali_err = lua_driver_adc_try_calibrate(ud);
    if (ud->cali_err != ESP_OK) {
        // Raw reads remain available when calibration is unsupported.
        ESP_LOGW(TAG, "Calibration unavailable for GPIO %d: %s", gpio, esp_err_to_name(ud->cali_err));
    }

    return 1;
}

static int lua_driver_adc_read_raw(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    int raw = 0;
    esp_err_t err = adc_oneshot_read(ud->unit, ud->channel, &raw);
    if (err != ESP_OK) {
        return luaL_error(L, "adc read failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, raw);
    return 1;
}

static int lua_driver_adc_read_mv(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    if (ud->cali == NULL) {
        return luaL_error(L, "adc calibration unavailable: %s", esp_err_to_name(ud->cali_err));
    }
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

static int lua_driver_adc_is_calibrated(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    lua_pushboolean(L, ud->cali != NULL);
    return 1;
}

static int lua_driver_adc_get_config(lua_State *L)
{
    lua_driver_adc_ud_t *ud = lua_driver_adc_get_ud(L, 1);
    adc_bitwidth_t bitwidth = ud->bitwidth == ADC_BITWIDTH_DEFAULT ? SOC_ADC_RTC_MAX_BITWIDTH : ud->bitwidth;
    lua_newtable(L);
    lua_pushinteger(L, ud->gpio_num);
    lua_setfield(L, -2, "gpio");
    lua_pushinteger(L, ud->atten);
    lua_setfield(L, -2, "atten");
    lua_pushinteger(L, bitwidth);
    lua_setfield(L, -2, "bitwidth");
    return 1;
}

static esp_err_t lua_driver_adc_close_ud(lua_driver_adc_ud_t *ud)
{
    ud->closing = true;
    esp_err_t cali_err = lua_driver_adc_release_cali(ud);
    if (cali_err != ESP_OK) return cali_err;
    if (ud->unit != NULL) {
        esp_err_t err = lua_driver_adc_release_channel(ud->gpio_num, ud->unit_id);
        if (err != ESP_OK) return err;
        ud->unit = NULL;
    }
    return cap_lua_hw_release(&ud->lease);
}

static int lua_driver_adc_gc(lua_State *L)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_ADC_METATABLE);
    if (ud) {
        lua_driver_adc_close_ud(ud);
    }
    return 0;
}

static int lua_driver_adc_close(lua_State *L)
{
    lua_driver_adc_ud_t *ud = (lua_driver_adc_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_ADC_METATABLE);
    esp_err_t err = lua_driver_adc_close_ud(ud);
    if (err != ESP_OK) return luaL_error(L, "adc close failed: %s", esp_err_to_name(err));
    return 0;
}

static const luaL_Reg s_channel_methods[] = {
    {"__gc", lua_driver_adc_gc},
    {"__close", lua_driver_adc_close},
    {"read_raw", lua_driver_adc_read_raw},
    {"read_mv", lua_driver_adc_read_mv},
    {"is_calibrated", lua_driver_adc_is_calibrated},
    {"get_config", lua_driver_adc_get_config},
    {"close", lua_driver_adc_close},
    {NULL, NULL},
};

static const luaL_Reg s_module_functions[] = {
    {"new", lua_driver_adc_new},
    {NULL, NULL},
};

static void lua_driver_adc_set_integer(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

int luaopen_adc(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_ADC_METATABLE)) {
        luaL_setfuncs(L, s_channel_methods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, s_module_functions, 0);
    lua_driver_adc_set_integer(L, "ATTEN_DB_0", ADC_ATTEN_DB_0);
    lua_driver_adc_set_integer(L, "ATTEN_DB_2_5", ADC_ATTEN_DB_2_5);
    lua_driver_adc_set_integer(L, "ATTEN_DB_6", ADC_ATTEN_DB_6);
    lua_driver_adc_set_integer(L, "ATTEN_DB_12", ADC_ATTEN_DB_12);
    lua_driver_adc_set_integer(L, "BITWIDTH_DEFAULT", ADC_BITWIDTH_DEFAULT);
    lua_driver_adc_set_integer(L, "BITWIDTH_9", ADC_BITWIDTH_9);
    lua_driver_adc_set_integer(L, "BITWIDTH_10", ADC_BITWIDTH_10);
    lua_driver_adc_set_integer(L, "BITWIDTH_11", ADC_BITWIDTH_11);
    lua_driver_adc_set_integer(L, "BITWIDTH_12", ADC_BITWIDTH_12);
    lua_driver_adc_set_integer(L, "BITWIDTH_13", ADC_BITWIDTH_13);
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
