/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_imu.h"

#include <stdlib.h>
#include <string.h>
#include "cap_lua.h"
#include "cap_lua_hw.h"
#include "driver/gpio.h"
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua_driver_i2c.h"
#include "lua_module_imu_backend.h"
#include "sdkconfig.h"

#define LUA_IMU_METATABLE "imu.device"
#define LUA_IMU_NAME_SIZE 64
#define LUA_IMU_DEFAULT_FREQUENCY 400000

static const char *TAG = "lua_module_imu";

typedef struct {
    lua_imu_backend_ctx_t ctx;
    const lua_imu_backend_t *backend;
    lua_driver_i2c_bus_handle_t bus_owner;
    cap_lua_hw_lease_t lease;
    char name[LUA_IMU_NAME_SIZE];
    int int_gpio;
    int sdo_gpio;
    bool borrowed;
    bool ready;
} lua_imu_ud_t;

static const lua_imu_backend_t *find_backend(const char *chip)
{
    if (!chip) return NULL;
#ifdef CONFIG_LUA_MODULE_IMU_CHIP_BMI270
    if (strcmp(chip, "bmi270") == 0) return &lua_imu_backend_bmi270;
#endif
#ifdef CONFIG_LUA_MODULE_IMU_CHIP_ICM42670
    if (strcmp(chip, "icm42670") == 0) return &lua_imu_backend_icm42670;
#endif
#ifdef CONFIG_LUA_MODULE_IMU_CHIP_MPU6050
    if (strcmp(chip, "mpu6050") == 0) return &lua_imu_backend_mpu6050;
#endif
    return NULL;
}

esp_err_t lua_imu_ctx_select_addr(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    if (ctx->i2c_dev_handle != NULL && ctx->i2c_addr == i2c_addr) {
        return ESP_OK;
    }
    if (ctx->i2c_dev_handle != NULL) {
        esp_err_t err = i2c_master_bus_rm_device(ctx->i2c_dev_handle);
        if (err != ESP_OK) return err;
        ctx->i2c_dev_handle = NULL;
    }
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = i2c_addr, .scl_speed_hz = ctx->frequency,
    };
    esp_err_t err = i2c_master_bus_add_device(ctx->master_bus, &config, &ctx->i2c_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create IMU I2C device for address 0x%02x", i2c_addr);
        return ESP_FAIL;
    }
    ctx->i2c_addr = i2c_addr;
    return ESP_OK;
}

static esp_err_t lua_imu_configure_interrupt_pin(int int_gpio_num)
{
    if (int_gpio_num < 0) {
        return ESP_OK;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << int_gpio_num,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

/*
 * Configure the SDO/AD0 strap GPIO to the given level. If `sdo_gpio_num` is
 * negative we treat the pin as not configured and intentionally do NOT touch
 * any GPIO -- this is critical: an unspecified SDO must never default to
 * GPIO0, which would otherwise be reconfigured as a sensor strap.
 */
static esp_err_t lua_imu_configure_sdo_pin(int sdo_gpio_num, int level)
{
    if (sdo_gpio_num < 0) {
        return ESP_OK;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << sdo_gpio_num,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level((gpio_num_t)sdo_gpio_num, level ? 1 : 0);
}

static esp_err_t lua_imu_release(lua_imu_ud_t *ud)
{
    ud->ready = false;
    if (!ud->borrowed) {
        esp_err_t err = ud->backend && ud->backend->destroy ? ud->backend->destroy(&ud->ctx) : ESP_OK;
        if (err != ESP_OK) return err;
        if (ud->ctx.i2c_dev_handle) {
            err = i2c_master_bus_rm_device(ud->ctx.i2c_dev_handle);
            if (err != ESP_OK) return err;
            ud->ctx.i2c_dev_handle = NULL;
        }
        if (ud->int_gpio >= 0) {
            err = gpio_reset_pin(ud->int_gpio);
            if (err != ESP_OK) return err;
            ud->int_gpio = -1;
        }
        if (ud->sdo_gpio >= 0) {
            err = gpio_reset_pin(ud->sdo_gpio);
            if (err != ESP_OK) return err;
            ud->sdo_gpio = -1;
        }
    }
    free(ud->ctx.state);
    ud->ctx.state = NULL;
    ud->ctx.sensor = NULL;
    lua_driver_i2c_unref_bus(ud->bus_owner);
    ud->bus_owner = NULL;
    return cap_lua_hw_release(&ud->lease);
}

static lua_imu_ud_t *lua_imu_get_ud(lua_State *L, int index)
{
    lua_imu_ud_t *ud = luaL_checkudata(L, index, LUA_IMU_METATABLE);
    luaL_argcheck(L, ud->ready, index, "invalid or closed IMU handle");
    return ud;
}

static int lua_imu_close(lua_State *L)
{
    lua_imu_ud_t *ud = luaL_checkudata(L, 1, LUA_IMU_METATABLE);
    esp_err_t err = lua_imu_release(ud);
    if (err != ESP_OK) return luaL_error(L, "imu close failed: %s", esp_err_to_name(err));
    lua_pushnil(L);
    lua_setuservalue(L, 1);
    return 0;
}

static int lua_imu_gc(lua_State *L)
{
    lua_imu_ud_t *ud = luaL_checkudata(L, 1, LUA_IMU_METATABLE);
    esp_err_t err = lua_imu_release(ud);
    if (err != ESP_OK) ESP_LOGE(TAG, "IMU cleanup failed: %s", esp_err_to_name(err));
    return 0;
}

static void lua_imu_push_axes_table(lua_State *L, int x, int y, int z)
{
    lua_newtable(L);
    lua_pushinteger(L, x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, z);
    lua_setfield(L, -2, "z");
}

static int lua_imu_name(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    lua_pushstring(L, ud->name);
    return 1;
}

static int lua_imu_read(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    lua_imu_sample_t sample = { 0 };
    if (ud->backend->read_sample(&ud->ctx, &sample) != ESP_OK) {
        return luaL_error(L, "imu read failed");
    }

    lua_newtable(L);
    lua_imu_push_axes_table(L, sample.accel.x, sample.accel.y, sample.accel.z);
    lua_setfield(L, -2, "accel");
    lua_imu_push_axes_table(L, sample.gyro.x, sample.gyro.y, sample.gyro.z);
    lua_setfield(L, -2, "gyro");
    lua_pushinteger(L, (lua_Integer)sample.sens_time);
    lua_setfield(L, -2, "sens_time");
    lua_pushinteger(L, (lua_Integer)sample.status);
    lua_setfield(L, -2, "status");
    return 1;
}

static int lua_imu_read_temperature(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    int32_t temp = 0;
    if (ud->backend->read_temperature(&ud->ctx, &temp) != ESP_OK) {
        return luaL_error(L, "imu read_temperature failed");
    }
    lua_pushinteger(L, temp);
    return 1;
}

static int lua_imu_read_int_status(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    uint32_t status = 0;
    if (ud->backend->read_int_status(&ud->ctx, &status) != ESP_OK) {
        return luaL_error(L, "imu read_int_status failed");
    }
    lua_pushinteger(L, (lua_Integer)status);
    return 1;
}

static int option_int(lua_State *L, const char *key, int fallback, int min, int max)
{
    lua_getfield(L, 1, key);
    lua_Integer value = luaL_optinteger(L, -1, fallback);
    lua_pop(L, 1);
    if (value < min || value > max) luaL_error(L, "imu.new: invalid %s", key);
    return (int)value;
}

static const char *board_chip(const char *name)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    for (const esp_board_device_desc_t *desc = g_esp_board_devices; desc && desc->name; desc = desc->next) {
        if (strcmp(desc->name, name) == 0) return desc->chip;
    }
    return NULL;
}

static int lua_imu_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argcheck(L, lua_gettop(L) == 1, 1, "expected one config table");
    lua_getfield(L, 1, "device");
    bool borrowed = !lua_isnil(L, -1);
    if (borrowed) luaL_checktype(L, -1, LUA_TSTRING);
    const char *name = borrowed ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        bool valid = key && (borrowed ? strcmp(key, "device") == 0 :
            strcmp(key, "chip") == 0 || strcmp(key, "bus") == 0 || strcmp(key, "address") == 0 ||
            strcmp(key, "int_gpio") == 0 || strcmp(key, "sdo_gpio") == 0);
        if (!valid) return luaL_error(L, "imu.new: unsupported or mixed config fields");
        lua_pop(L, 1);
    }
    const char *chip;
    if (borrowed) chip = board_chip(name);
    else {
        lua_getfield(L, 1, "chip");
        luaL_checktype(L, -1, LUA_TSTRING);
        chip = lua_tostring(L, -1);
        lua_pop(L, 1);
        name = chip;
    }
    const lua_imu_backend_t *backend = find_backend(chip);
    if (!backend) return luaL_error(L, "imu.new: device missing or chip backend not enabled");
    if (!name[0] || strlen(name) >= LUA_IMU_NAME_SIZE) return luaL_error(L, "imu.new: invalid device name");

    lua_imu_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_imu_ud_t){.backend = backend, .borrowed = borrowed, .int_gpio = -1, .sdo_gpio = -1};
    strcpy(ud->name, name);
    luaL_getmetatable(L, LUA_IMU_METATABLE);
    lua_setmetatable(L, -2);
    int object = lua_gettop(L);
    esp_err_t err;
    if (borrowed) {
        char key[CLAW_HW_KEY_SIZE];
        const char *keys[] = {claw_hw_key_device(key, sizeof(key), name)};
        err = cap_lua_hw_claim(L, &ud->lease, "lua/imu", keys, 1);
        if (err == ESP_OK) {
            err = esp_board_manager_get_device_handle(name, &ud->ctx.sensor);
            if (err != ESP_OK || !ud->ctx.sensor) {
                err = esp_board_manager_init_device_by_name(name);
                if (err == ESP_OK) err = esp_board_manager_get_device_handle(name, &ud->ctx.sensor);
            }
            if (err == ESP_OK && !ud->ctx.sensor) err = ESP_ERR_INVALID_STATE;
        }
    } else {
        int address = option_int(L, "address", backend->default_addr(), 0, 0x7f);
        int int_gpio = option_int(L, "int_gpio", -1, -1, GPIO_NUM_MAX - 1);
        int sdo_gpio = option_int(L, "sdo_gpio", -1, -1, GPIO_NUM_MAX - 1);
        if (!backend->is_supported_addr(address) || (int_gpio >= 0 && !GPIO_IS_VALID_GPIO(int_gpio)) ||
            (sdo_gpio >= 0 && (!GPIO_IS_VALID_OUTPUT_GPIO(sdo_gpio) || sdo_gpio == int_gpio))) {
            return luaL_error(L, "imu.new: unsupported address or invalid GPIO");
        }
        lua_getfield(L, 1, "bus");
        lua_pushvalue(L, -1);
        lua_setuservalue(L, object);
        int port;
        ud->ctx.master_bus = lua_driver_i2c_ref_bus(L, -1, &port, &ud->bus_owner);
        lua_pop(L, 1);
        ud->ctx.frequency = LUA_IMU_DEFAULT_FREQUENCY;
        char key_buf[3][24];
        const char *keys[3];
        size_t count = 0;
        keys[count] = claw_hw_key_i2c_addr(key_buf[count], sizeof(key_buf[count]), port, address);
        ++count;
        if (int_gpio >= 0) {
            keys[count] = claw_hw_key_gpio(key_buf[count], sizeof(key_buf[count]), int_gpio);
            ++count;
        }
        if (sdo_gpio >= 0) {
            keys[count] = claw_hw_key_gpio(key_buf[count], sizeof(key_buf[count]), sdo_gpio);
            ++count;
        }
        err = cap_lua_hw_claim(L, &ud->lease, "lua/imu", keys, count);
        if (err == ESP_OK) {
            ud->int_gpio = int_gpio;
            ud->sdo_gpio = sdo_gpio;
            if (backend->state_size) {
                ud->ctx.state = calloc(1, backend->state_size);
                if (!ud->ctx.state) err = ESP_ERR_NO_MEM;
            }
            if (err == ESP_OK) err = lua_imu_configure_interrupt_pin(int_gpio);
            int level = backend->sdo_level_for_addr ? backend->sdo_level_for_addr(address) : 0;
            if (err == ESP_OK) err = lua_imu_configure_sdo_pin(sdo_gpio, level);
            if (err == ESP_OK) err = backend->probe(&ud->ctx, address);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "IMU '%s' open failed: %s", name, esp_err_to_name(err));
        (void)lua_imu_release(ud);
        return luaL_error(L, "imu.new failed: %s", esp_err_to_name(err));
    }
    ud->ready = true;
    return 1;
}

int luaopen_imu(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_IMU_METATABLE)) {
        lua_pushcfunction(L, lua_imu_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_imu_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_imu_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_imu_read_int_status);
        lua_setfield(L, -2, "read_int_status");
        lua_pushcfunction(L, lua_imu_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_imu_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_imu_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_imu_register(void)
{
    return cap_lua_register_module("imu", luaopen_imu);
}
