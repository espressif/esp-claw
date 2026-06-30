/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_i2c.h"

#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#define LUA_DRIVER_I2C_BUS_METATABLE    "i2c.bus"
#define LUA_DRIVER_I2C_DEVICE_METATABLE "i2c.device"
#define LUA_DRIVER_I2C_DEFAULT_FREQ_HZ  400000U
#define LUA_DRIVER_I2C_SCAN_MAX         128
#define LUA_DRIVER_I2C_RW_MAX_LEN       1024

typedef struct {
    i2c_bus_handle_t bus;
    int port;
    bool external_owned;
} lua_driver_i2c_bus_ud_t;

typedef struct {
    i2c_bus_device_handle_t dev;
    uint8_t addr;
    int bus_ref;
} lua_driver_i2c_device_ud_t;

static lua_driver_i2c_bus_ud_t *lua_driver_i2c_bus_get_ud(lua_State *L, int idx)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2C_BUS_METATABLE);
    if (!ud || !ud->bus) {
        luaL_error(L, "i2c bus: invalid or closed handle");
    }
    return ud;
}

static lua_driver_i2c_device_ud_t *lua_driver_i2c_device_get_ud(lua_State *L, int idx)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (!ud || !ud->dev) {
        luaL_error(L, "i2c device: invalid or closed handle");
    }
    return ud;
}

static uint8_t lua_driver_i2c_mem_addr(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) {
        return NULL_I2C_MEM_ADDR;
    }
    lua_Integer v = luaL_checkinteger(L, idx);
    if (v < 0 || v > 0xFF) {
        luaL_error(L, "mem_addr must be in range 0-255");
    }
    return (uint8_t)v;
}

static esp_err_t lua_driver_i2c_bus_release(lua_driver_i2c_bus_ud_t *ud)
{
    if (ud == NULL || ud->bus == NULL) {
        return ESP_OK;
    }
    if (ud->external_owned) {
        ud->bus = NULL;
        return ESP_OK;
    }
    return i2c_bus_delete(&ud->bus);
}

static int lua_driver_i2c_bus_gc(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_BUS_METATABLE);
    if (ud && ud->bus) {
        (void)lua_driver_i2c_bus_release(ud);
    }
    return 0;
}

static int lua_driver_i2c_bus_close(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_BUS_METATABLE);
    if (ud->bus) {
        esp_err_t err = lua_driver_i2c_bus_release(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "i2c bus close failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_driver_i2c_bus_scan(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);
    uint8_t buf[LUA_DRIVER_I2C_SCAN_MAX];
    uint8_t count = i2c_bus_scan(ud->bus, buf, LUA_DRIVER_I2C_SCAN_MAX);
    lua_createtable(L, count, 0);
    for (uint8_t i = 0; i < count; i++) {
        lua_pushinteger(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int lua_driver_i2c_bus_device(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);
    lua_Integer addr = luaL_checkinteger(L, 2);
    uint32_t clk_speed = (uint32_t)luaL_optinteger(L, 3, 0);

    if (addr < 0 || addr > 0x7F) {
        return luaL_error(L, "i2c address must be in range 0-127");
    }

    i2c_bus_device_handle_t dev = i2c_bus_device_create(ud->bus, (uint8_t)addr, clk_speed);
    if (!dev) {
        return luaL_error(L, "i2c device create failed");
    }

    lua_driver_i2c_device_ud_t *dud = (lua_driver_i2c_device_ud_t *)lua_newuserdata(
        L, sizeof(*dud));
    dud->dev = dev;
    dud->addr = (uint8_t)addr;
    dud->bus_ref = LUA_NOREF;
    luaL_getmetatable(L, LUA_DRIVER_I2C_DEVICE_METATABLE);
    lua_setmetatable(L, -2);

    lua_pushvalue(L, 1);
    dud->bus_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    return 1;
}

static int lua_driver_i2c_device_gc(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (ud) {
        if (ud->dev) {
            i2c_bus_device_delete(&ud->dev);
        }
        if (ud->bus_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->bus_ref);
            ud->bus_ref = LUA_NOREF;
        }
    }
    return 0;
}

static int lua_driver_i2c_device_close(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (ud->dev) {
        esp_err_t err = i2c_bus_device_delete(&ud->dev);
        if (err != ESP_OK) {
            return luaL_error(L, "i2c device close failed: %s", esp_err_to_name(err));
        }
    }
    if (ud->bus_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->bus_ref);
        ud->bus_ref = LUA_NOREF;
    }
    return 0;
}

static int lua_driver_i2c_device_address(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_pushinteger(L, ud->addr);
    return 1;
}

static int lua_driver_i2c_device_read_byte(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 2);
    uint8_t data = 0;
    esp_err_t err = i2c_bus_read_byte(ud->dev, mem_addr, &data);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c read_byte failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, data);
    return 1;
}

static int lua_driver_i2c_device_read(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_Integer len = luaL_checkinteger(L, 2);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 3);

    if (len <= 0 || len > LUA_DRIVER_I2C_RW_MAX_LEN) {
        return luaL_error(L, "i2c read length must be 1-%d", LUA_DRIVER_I2C_RW_MAX_LEN);
    }

    luaL_Buffer b;
    uint8_t *buf = (uint8_t *)luaL_buffinitsize(L, &b, (size_t)len);
    esp_err_t err = i2c_bus_read_bytes(ud->dev, mem_addr, (size_t)len, buf);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c read failed: %s", esp_err_to_name(err));
    }
    luaL_pushresultsize(&b, (size_t)len);
    return 1;
}

static int lua_driver_i2c_device_write_byte(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_Integer value = luaL_checkinteger(L, 2);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 3);

    if (value < 0 || value > 0xFF) {
        return luaL_error(L, "i2c write_byte value must be 0-255");
    }

    esp_err_t err = i2c_bus_write_byte(ud->dev, mem_addr, (uint8_t)value);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c write_byte failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_i2c_device_write(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 3);

    const uint8_t *data = NULL;
    size_t data_len = 0;

    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        data = (const uint8_t *)lua_tolstring(L, 2, &data_len);
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 2);
        if (n < 0 || n > LUA_DRIVER_I2C_RW_MAX_LEN) {
            return luaL_error(L, "i2c write table length must be 0-%d",
                              LUA_DRIVER_I2C_RW_MAX_LEN);
        }
        uint8_t *tmp = (uint8_t *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);
            if (byte < 0 || byte > 0xFF) {
                return luaL_error(L, "i2c write byte #%d out of range 0-255",
                                  (int)(i + 1));
            }
            tmp[i] = (uint8_t)byte;
            lua_pop(L, 1);
        }
        data = tmp;
        data_len = (size_t)n;
    } else {
        return luaL_error(L, "i2c write expects a string or table");
    }

    if (data_len == 0) {
        return 0;
    }
    if (data_len > LUA_DRIVER_I2C_RW_MAX_LEN) {
        return luaL_error(L, "i2c write length must be 1-%d", LUA_DRIVER_I2C_RW_MAX_LEN);
    }

    esp_err_t err = i2c_bus_write_bytes(ud->dev, mem_addr, data_len, data);
    if (err != ESP_OK) {
        return luaL_error(L, "i2c write failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_i2c_new(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    lua_Integer sda = luaL_checkinteger(L, 2);
    lua_Integer scl = luaL_checkinteger(L, 3);
    lua_Integer freq = luaL_optinteger(L, 4, LUA_DRIVER_I2C_DEFAULT_FREQ_HZ);

    if (freq <= 0) {
        return luaL_error(L, "i2c freq must be positive");
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = (int)sda,
        .scl_io_num = (int)scl,
        .sda_pullup_en = true,
        .scl_pullup_en = true,
        .master.clk_speed = (uint32_t)freq,
    };

    i2c_master_bus_handle_t existing_bus = NULL;
    bool external_owned = i2c_master_get_bus_handle((i2c_port_t)port, &existing_bus) == ESP_OK;

    i2c_bus_handle_t bus = i2c_bus_create((i2c_port_t)port, &conf);
    if (!bus) {
        return luaL_error(L, "i2c bus create failed on port %d", (int)port);
    }

    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->bus = bus;
    ud->port = (int)port;
    ud->external_owned = external_owned;
    luaL_getmetatable(L, LUA_DRIVER_I2C_BUS_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_i2c(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_I2C_BUS_METATABLE)) {
        lua_pushcfunction(L, lua_driver_i2c_bus_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_bus_scan);
        lua_setfield(L, -2, "scan");
        lua_pushcfunction(L, lua_driver_i2c_bus_device);
        lua_setfield(L, -2, "device");
        lua_pushcfunction(L, lua_driver_i2c_bus_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, LUA_DRIVER_I2C_DEVICE_METATABLE)) {
        lua_pushcfunction(L, lua_driver_i2c_device_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_i2c_device_read_byte);
        lua_setfield(L, -2, "read_byte");
        lua_pushcfunction(L, lua_driver_i2c_device_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_i2c_device_write_byte);
        lua_setfield(L, -2, "write_byte");
        lua_pushcfunction(L, lua_driver_i2c_device_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_i2c_device_address);
        lua_setfield(L, -2, "address");
        lua_pushcfunction(L, lua_driver_i2c_device_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_i2c_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_i2c_register(void)
{
    return cap_lua_register_module("i2c", luaopen_i2c);
}
