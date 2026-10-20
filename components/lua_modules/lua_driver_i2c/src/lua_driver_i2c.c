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
#include "cap_lua_hw.h"
#include "driver/gpio.h"
#include "soc/soc_caps.h"

#define LUA_DRIVER_I2C_BUS_METATABLE    "i2c.bus"
#define LUA_DRIVER_I2C_DEVICE_METATABLE "i2c.device"
#define LUA_DRIVER_I2C_DEFAULT_FREQ_HZ  400000U
#define LUA_DRIVER_I2C_SCAN_MAX         128
#define LUA_DRIVER_I2C_RW_MAX_LEN       1024

typedef struct lua_driver_i2c_bus {
    i2c_master_bus_handle_t bus;
    int port;
    bool external_owned;
    unsigned devices;
    uint32_t frequency;
    cap_lua_hw_lease_t lease;
    bool closing;
} lua_driver_i2c_bus_ud_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    lua_driver_i2c_bus_ud_t *parent;
    cap_lua_hw_lease_t lease;
    bool closing;
} lua_driver_i2c_device_ud_t;

static lua_driver_i2c_bus_ud_t *lua_driver_i2c_bus_get_ud(lua_State *L, int idx)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2C_BUS_METATABLE);
    if (!ud || !ud->bus || ud->closing) {
        luaL_error(L, "i2c bus: invalid or closed handle");
    }
    return ud;
}

static lua_driver_i2c_device_ud_t *lua_driver_i2c_device_get_ud(lua_State *L, int idx)
{
    lua_driver_i2c_device_ud_t *ud = (lua_driver_i2c_device_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_I2C_DEVICE_METATABLE);
    if (!ud || !ud->dev || ud->closing) {
        luaL_error(L, "i2c device: invalid or closed handle");
    }
    return ud;
}

i2c_master_bus_handle_t lua_driver_i2c_ref_bus(lua_State *L, int index, int *port, lua_driver_i2c_bus_handle_t *owner)
{
    lua_driver_i2c_bus_ud_t *bus = lua_driver_i2c_bus_get_ud(L, index);
    ++bus->devices;
    *port = bus->port;
    *owner = bus;
    return bus->bus;
}

void lua_driver_i2c_unref_bus(lua_driver_i2c_bus_handle_t owner)
{
    if (owner) --owner->devices;
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
    if (!ud) return ESP_OK;
    if (ud->devices) return ESP_ERR_INVALID_STATE;
    ud->closing = true;
    if (ud->bus && !ud->external_owned) {
        esp_err_t err = i2c_del_master_bus(ud->bus);
        if (err != ESP_OK) return err;
        ud->bus = NULL;
    }
    ud->bus = NULL;
    return cap_lua_hw_release(&ud->lease);
}

static esp_err_t lua_driver_i2c_device_release(lua_driver_i2c_device_ud_t *ud)
{
    ud->closing = true;
    if (ud->dev) {
        esp_err_t err = i2c_master_bus_rm_device(ud->dev);
        if (err != ESP_OK) return err;
        ud->dev = NULL;
    }
    esp_err_t err = cap_lua_hw_release(&ud->lease);
    if (err == ESP_OK && ud->parent) {
        ud->parent->devices--;
        ud->parent = NULL;
    }
    return err;
}

static int lua_driver_i2c_bus_gc(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = (lua_driver_i2c_bus_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_I2C_BUS_METATABLE);
    if (ud) {
        (void)lua_driver_i2c_bus_release(ud);
    }
    return 0;
}

static int lua_driver_i2c_bus_close(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = luaL_checkudata(L, 1, LUA_DRIVER_I2C_BUS_METATABLE);
    esp_err_t err = lua_driver_i2c_bus_release(ud);
    if (err != ESP_OK) return luaL_error(L, "i2c bus close failed: %s", esp_err_to_name(err));
    return 0;
}

static int lua_driver_i2c_bus_scan(lua_State *L)
{
    lua_driver_i2c_bus_ud_t *ud = lua_driver_i2c_bus_get_ud(L, 1);
    uint8_t buf[LUA_DRIVER_I2C_SCAN_MAX];
    uint8_t count = 0;
    for (int addr = 1; addr < 127; ++addr) {
        if (i2c_master_probe(ud->bus, addr, 50) == ESP_OK) buf[count++] = addr;
    }
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

    lua_driver_i2c_device_ud_t *dud = lua_newuserdata(L, sizeof(*dud));
    *dud = (lua_driver_i2c_device_ud_t) { .addr = (uint8_t)addr };
    luaL_getmetatable(L, LUA_DRIVER_I2C_DEVICE_METATABLE);
    lua_setmetatable(L, -2);
    char key[CLAW_HW_KEY_SIZE];
    const char *keys[] = { claw_hw_key_i2c_addr(key, sizeof(key), ud->port, (int)addr) };
    esp_err_t err = cap_lua_hw_claim(L, &dud->lease, "lua/i2c-device", keys, 1);
    if (err != ESP_OK) return luaL_error(L, "i2c address claim failed: %s", esp_err_to_name(err));
    /* Keep the bus alive while this device uses it. */
    lua_pushvalue(L, 1);
    lua_setuservalue(L, -2);
    dud->parent = ud;
    ud->devices++;
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = clk_speed ? clk_speed : ud->frequency };
    err = i2c_master_bus_add_device(ud->bus, &cfg, &dud->dev);
    if (err != ESP_OK) {
        (void)lua_driver_i2c_device_release(dud);
        return luaL_error(L, "i2c device create failed");
    }
    return 1;
}

static int lua_driver_i2c_device_close(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = luaL_checkudata(L, 1, LUA_DRIVER_I2C_DEVICE_METATABLE);
    esp_err_t err = lua_driver_i2c_device_release(ud);
    if (err != ESP_OK) return luaL_error(L, "i2c device close failed: %s", esp_err_to_name(err));
    lua_pushnil(L);
    lua_setuservalue(L, 1);
    return 0;
}

static int lua_driver_i2c_device_gc(lua_State *L)
{
    return lua_driver_i2c_device_close(L);
}

static int lua_driver_i2c_device_address(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    lua_pushinteger(L, ud->addr);
    return 1;
}

static esp_err_t lua_i2c_read(i2c_master_dev_handle_t dev, uint8_t address, size_t size, uint8_t *data)
{
    if (address == NULL_I2C_MEM_ADDR) return i2c_master_receive(dev, data, size, 1000);
    return i2c_master_transmit_receive(dev, &address, 1, data, size, 1000);
}

static esp_err_t lua_i2c_write(i2c_master_dev_handle_t dev, uint8_t address, size_t size, const uint8_t *data)
{
    if (address == NULL_I2C_MEM_ADDR) return i2c_master_transmit(dev, data, size, 1000);
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        { .write_buffer = &address, .buffer_size = 1 },
        { .write_buffer = (uint8_t *)data, .buffer_size = size },
    };
    return i2c_master_multi_buffer_transmit(dev, buffers, 2, 1000);
}

static int lua_driver_i2c_device_read_byte(lua_State *L)
{
    lua_driver_i2c_device_ud_t *ud = lua_driver_i2c_device_get_ud(L, 1);
    uint8_t mem_addr = lua_driver_i2c_mem_addr(L, 2);
    uint8_t data = 0;
    esp_err_t err = lua_i2c_read(ud->dev, mem_addr, 1, &data);
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
    esp_err_t err = lua_i2c_read(ud->dev, mem_addr, (size_t)len, buf);
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

    uint8_t data = value;
    esp_err_t err = lua_i2c_write(ud->dev, mem_addr, 1, &data);
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

    esp_err_t err = lua_i2c_write(ud->dev, mem_addr, data_len, data);
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

    if (freq <= 0 || (uint64_t)freq > UINT32_MAX) {
        return luaL_error(L, "i2c freq must be positive");
    }

    i2c_master_bus_config_t conf = {
        .i2c_port = (int)port, .sda_io_num = (int)sda, .scl_io_num = (int)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    luaL_argcheck(L, port >= 0 && port < SOC_I2C_NUM, 1, "invalid I2C port");
    luaL_argcheck(L, sda >= 0 && sda < SOC_GPIO_PIN_COUNT && GPIO_IS_VALID_OUTPUT_GPIO(sda), 2, "invalid SDA GPIO");
    luaL_argcheck(L, scl >= 0 && scl < SOC_GPIO_PIN_COUNT && GPIO_IS_VALID_OUTPUT_GPIO(scl), 3, "invalid SCL GPIO");
    lua_driver_i2c_bus_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_driver_i2c_bus_ud_t) { .port = (int)port, .frequency = (uint32_t)freq };
    luaL_getmetatable(L, LUA_DRIVER_I2C_BUS_METATABLE);
    lua_setmetatable(L, -2);
    char keys_buf[3][24];
    const char *keys[] = {
        claw_hw_key_i2c_controller(keys_buf[0], sizeof(keys_buf[0]), (int)port),
        claw_hw_key_gpio(keys_buf[1], sizeof(keys_buf[1]), (int)sda),
        claw_hw_key_gpio(keys_buf[2], sizeof(keys_buf[2]), (int)scl),
    };
    esp_err_t err = cap_lua_hw_claim(L, &ud->lease, "lua/i2c-bus", keys, 3);
    if (err != ESP_OK) return luaL_error(L, "i2c bus claim failed: %s", esp_err_to_name(err));
    i2c_master_bus_handle_t existing = NULL;
    if (i2c_master_get_bus_handle((i2c_port_t)port, &existing) == ESP_OK) {
        (void)lua_driver_i2c_bus_release(ud);
        return luaL_error(L, "I2C bus already initialized; use i2c.borrow");
    }
    err = i2c_new_master_bus(&conf, &ud->bus);
    if (err != ESP_OK) {
        (void)lua_driver_i2c_bus_release(ud);
        return luaL_error(L, "i2c bus create failed on port %d", (int)port);
    }
    return 1;
}

static int lua_driver_i2c_borrow(lua_State *L)
{
    int port = luaL_checkinteger(L, 1);
    luaL_argcheck(L, port >= 0 && port < SOC_I2C_NUM, 1, "invalid I2C port");
    lua_driver_i2c_bus_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_driver_i2c_bus_ud_t) { .port = port, .external_owned = true, .frequency = LUA_DRIVER_I2C_DEFAULT_FREQ_HZ };
    luaL_getmetatable(L, LUA_DRIVER_I2C_BUS_METATABLE);
    lua_setmetatable(L, -2);
    /* The board keeps this bus alive throughout Lua use. */
    esp_err_t err = i2c_master_get_bus_handle(port, &ud->bus);
    if (err != ESP_OK || !ud->bus) {
        (void)lua_driver_i2c_bus_release(ud);
        return luaL_error(L, "I2C provider has no initialized bus");
    }
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
    lua_pushcfunction(L, lua_driver_i2c_borrow);
    lua_setfield(L, -2, "borrow");
    lua_pushcfunction(L, lua_driver_i2c_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_i2c_register(void)
{
    return cap_lua_register_module("i2c", luaopen_i2c);
}
