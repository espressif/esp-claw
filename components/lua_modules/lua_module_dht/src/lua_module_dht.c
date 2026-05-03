/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_dht.h"

#include <string.h>

#include "cap_lua.h"
#include "dht.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "lauxlib.h"

static dht_sensor_type_t lua_module_dht_sensor_type_from_string(const char *sensor_type_str)
{
    if (!sensor_type_str || strcmp(sensor_type_str, "dht11") == 0) {
        return DHT_TYPE_DHT11;
    }
    if (strcmp(sensor_type_str, "dht22") == 0 ||
        strcmp(sensor_type_str, "am2301") == 0 ||
        strcmp(sensor_type_str, "am2302") == 0 ||
        strcmp(sensor_type_str, "am2321") == 0 ||
        strcmp(sensor_type_str, "dht21") == 0) {
        return DHT_TYPE_AM2301;
    }
    if (strcmp(sensor_type_str, "si7021") == 0) {
        return DHT_TYPE_SI7021;
    }
    return (dht_sensor_type_t)-1;
}

static dht_sensor_type_t lua_module_dht_check_sensor_type(lua_State *L, int idx)
{
    const char *sensor_type_str = NULL;

    if (lua_gettop(L) < idx || lua_isnoneornil(L, idx)) {
        return DHT_TYPE_DHT11;
    }

    sensor_type_str = luaL_checkstring(L, idx);
    dht_sensor_type_t sensor_type = lua_module_dht_sensor_type_from_string(sensor_type_str);
    if ((int)sensor_type < 0) {
        luaL_error(L, "invalid dht sensor type: %s", sensor_type_str);
    }
    return sensor_type;
}

static int lua_module_dht_read(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);
    dht_sensor_type_t sensor_type = lua_module_dht_check_sensor_type(L, 2);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = dht_read_float_data(sensor_type, pin, &humidity, &temperature);
    if (err != ESP_OK) {
        return luaL_error(L, "dht_read_float_data failed: %s", esp_err_to_name(err));
    }

    lua_pushnumber(L, temperature);
    lua_pushnumber(L, humidity);
    return 2;
}

static int lua_module_dht_read_raw(lua_State *L)
{
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, 1);
    dht_sensor_type_t sensor_type = lua_module_dht_check_sensor_type(L, 2);
    int16_t humidity = 0;
    int16_t temperature = 0;

    esp_err_t err = dht_read_data(sensor_type, pin, &humidity, &temperature);
    if (err != ESP_OK) {
        return luaL_error(L, "dht_read_data failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, temperature);
    lua_pushinteger(L, humidity);
    return 2;
}

int luaopen_dht(lua_State *L)
{
    lua_newtable(L);

    lua_pushcfunction(L, lua_module_dht_read);
    lua_setfield(L, -2, "read");

    lua_pushcfunction(L, lua_module_dht_read_raw);
    lua_setfield(L, -2, "read_raw");

    lua_pushstring(L, "dht11");
    lua_setfield(L, -2, "TYPE_DHT11");

    lua_pushstring(L, "dht22");
    lua_setfield(L, -2, "TYPE_DHT22");

    lua_pushstring(L, "si7021");
    lua_setfield(L, -2, "TYPE_SI7021");

    return 1;
}

esp_err_t lua_module_dht_register(void)
{
    return cap_lua_register_module("dht", luaopen_dht);
}
