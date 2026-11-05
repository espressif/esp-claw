/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_magnetometer.h"

#include <float.h>
#include <math.h>
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
#include "lua_module_mag_backend.h"
#include "sdkconfig.h"

#define LUA_MAG_METATABLE "magnetometer.device"
#define LUA_MAG_NAME_SIZE 64
#define LUA_MAG_DEFAULT_FREQUENCY 400000
#define LUA_MAG_MIN_CALIBRATION_SAMPLES 16

static const char *TAG = "lua_module_magnetometer";

typedef struct {
    float hard_iron[3];
    float soft_iron[3][3];
    float mag_min[3];
    float mag_max[3];
    uint32_t sample_count;
    bool calibrated;
    bool collecting;
} lua_mag_calibration_t;

typedef struct {
    lua_mag_backend_ctx_t ctx;
    const lua_mag_backend_t *backend;
    lua_driver_i2c_bus_handle_t bus_owner;
    cap_lua_hw_lease_t lease;
    char name[LUA_MAG_NAME_SIZE];
    int int_gpio;
    int sdo_gpio;
    bool borrowed;
    bool ready;
    lua_mag_calibration_t calibration;
} lua_mag_ud_t;

static const lua_mag_backend_t *find_backend(const char *chip)
{
    if (!chip) return NULL;
#ifdef CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150
    if (strcmp(chip, "bmm150") == 0) return &lua_mag_backend_bmm150;
#endif
#ifdef CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350
    if (strcmp(chip, "bmm350") == 0) return &lua_mag_backend_bmm350;
#endif
#ifdef CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_QMC6309
    if (strcmp(chip, "qmc6309") == 0) return &lua_mag_backend_qmc6309;
#endif
    return NULL;
}

static esp_err_t lua_mag_configure_interrupt_pin(int int_gpio_num)
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

/* Only configure an explicitly supplied address strap. */
static esp_err_t lua_mag_configure_sdo_pin(int sdo_gpio_num, int level)
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

static esp_err_t lua_mag_release(lua_mag_ud_t *ud)
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
    ud->ctx.sensor = NULL;
    lua_driver_i2c_unref_bus(ud->bus_owner);
    ud->bus_owner = NULL;
    return cap_lua_hw_release(&ud->lease);
}

static lua_mag_ud_t *lua_mag_get_ud(lua_State *L, int index)
{
    lua_mag_ud_t *ud = luaL_checkudata(L, index, LUA_MAG_METATABLE);
    luaL_argcheck(L, ud->ready, index, "invalid or closed magnetometer handle");
    return ud;
}

static int lua_mag_close(lua_State *L)
{
    lua_mag_ud_t *ud = luaL_checkudata(L, 1, LUA_MAG_METATABLE);
    esp_err_t err = lua_mag_release(ud);
    if (err != ESP_OK) return luaL_error(L, "magnetometer close failed: %s", esp_err_to_name(err));
    lua_pushnil(L);
    lua_setuservalue(L, 1);
    return 0;
}

static int lua_mag_gc(lua_State *L)
{
    lua_mag_ud_t *ud = luaL_checkudata(L, 1, LUA_MAG_METATABLE);
    esp_err_t err = lua_mag_release(ud);
    if (err != ESP_OK) ESP_LOGE(TAG, "magnetometer cleanup failed: %s", esp_err_to_name(err));
    return 0;
}

static void lua_mag_calibration_reset_state(lua_mag_ud_t *handle)
{
    for (size_t i = 0; i < 3; i++) {
        handle->calibration.hard_iron[i] = 0.0f;
        handle->calibration.mag_min[i] = FLT_MAX;
        handle->calibration.mag_max[i] = -FLT_MAX;
        for (size_t j = 0; j < 3; j++) {
            handle->calibration.soft_iron[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
    handle->calibration.sample_count = 0;
    handle->calibration.calibrated = false;
    handle->calibration.collecting = false;
}

static void lua_mag_apply_calibration(const lua_mag_calibration_t *cal,
                                      const float raw[3], float corrected[3])
{
    if (!cal->calibrated) {
        corrected[0] = raw[0];
        corrected[1] = raw[1];
        corrected[2] = raw[2];
        return;
    }
    float v[3] = {
        raw[0] - cal->hard_iron[0],
        raw[1] - cal->hard_iron[1],
        raw[2] - cal->hard_iron[2],
    };
    for (size_t row = 0; row < 3; row++) {
        corrected[row] = cal->soft_iron[row][0] * v[0] +
                         cal->soft_iron[row][1] * v[1] +
                         cal->soft_iron[row][2] * v[2];
    }
}

static void lua_mag_calibration_record_sample(lua_mag_ud_t *handle, const float sample[3])
{
    for (size_t i = 0; i < 3; i++) {
        if (sample[i] < handle->calibration.mag_min[i]) {
            handle->calibration.mag_min[i] = sample[i];
        }
        if (sample[i] > handle->calibration.mag_max[i]) {
            handle->calibration.mag_max[i] = sample[i];
        }
    }
    handle->calibration.sample_count++;
    handle->calibration.collecting = true;
}

static esp_err_t lua_mag_calibration_finish(lua_mag_ud_t *handle)
{
    lua_mag_calibration_t *cal = &handle->calibration;
    float offset[3], radius[3], scale[3];
    if (cal->sample_count < LUA_MAG_MIN_CALIBRATION_SAMPLES) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < 3; i++) {
        offset[i] = cal->mag_max[i] * 0.5f + cal->mag_min[i] * 0.5f;
        radius[i] = cal->mag_max[i] * 0.5f - cal->mag_min[i] * 0.5f;
        if (!isfinite(radius[i]) || radius[i] <= 0.0f) return ESP_ERR_INVALID_STATE;
    }
    double average = ((double)radius[0] + radius[1] + radius[2]) / 3.0;
    for (size_t i = 0; i < 3; i++) {
        scale[i] = average / radius[i];
        if (!isfinite(scale[i])) return ESP_ERR_INVALID_STATE;
    }
    /* Publish calibration only after every axis passes validation. */
    memcpy(cal->hard_iron, offset, sizeof(offset));
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = 0; j < 3; j++) cal->soft_iron[i][j] = i == j ? scale[i] : 0.0f;
    }
    cal->calibrated = true;
    cal->collecting = false;
    return ESP_OK;
}

static void lua_mag_push_axes_table(lua_State *L, float x, float y, float z)
{
    lua_newtable(L);
    lua_pushnumber(L, x);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, y);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, z);
    lua_setfield(L, -2, "z");
}

static void lua_mag_push_calibration_table(lua_State *L, const lua_mag_calibration_t *cal)
{
    lua_newtable(L);
    lua_pushboolean(L, cal->calibrated);
    lua_setfield(L, -2, "calibrated");
    lua_pushboolean(L, cal->collecting);
    lua_setfield(L, -2, "collecting");
    lua_pushinteger(L, (lua_Integer)cal->sample_count);
    lua_setfield(L, -2, "sample_count");

    lua_newtable(L);
    for (size_t i = 0; i < 3; i++) {
        lua_pushnumber(L, cal->hard_iron[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "hard_iron");

    lua_newtable(L);
    for (size_t row = 0; row < 3; row++) {
        lua_newtable(L);
        for (size_t col = 0; col < 3; col++) {
            lua_pushnumber(L, cal->soft_iron[row][col]);
            lua_rawseti(L, -2, (int)col + 1);
        }
        lua_rawseti(L, -2, (int)row + 1);
    }
    lua_setfield(L, -2, "soft_iron");
}

static bool lua_mag_read_vec3(lua_State *L, int idx, float out[3])
{
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        lua_rawgeti(L, idx, i + 1);
        if (!lua_isnumber(L, -1) || !isfinite((float)lua_tonumber(L, -1))) {
            ok = false;
        } else {
            out[i] = (float)lua_tonumber(L, -1);
        }
        lua_pop(L, 1);
    }
    return ok;
}

static bool lua_mag_read_mat3(lua_State *L, int idx, float out[3][3])
{
    bool ok = true;
    for (int row = 0; row < 3 && ok; row++) {
        lua_rawgeti(L, idx, row + 1);
        if (!lua_istable(L, -1)) {
            ok = false;
        } else {
            for (int col = 0; col < 3; col++) {
                lua_rawgeti(L, -1, col + 1);
                if (!lua_isnumber(L, -1) || !isfinite((float)lua_tonumber(L, -1))) {
                    ok = false;
                } else {
                    out[row][col] = (float)lua_tonumber(L, -1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    return ok;
}

static int lua_mag_name(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_pushstring(L, ud->name);
    return 1;
}

static int lua_mag_chip_id(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_pushinteger(L, ud->backend->chip_id);
    return 1;
}

static int lua_mag_read(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_sample_t sample = { 0 };
    float raw[3];
    float corrected[3];

    esp_err_t err = ud->backend->read_sample(&ud->ctx, &sample);
    if (err != ESP_OK) return luaL_error(L, "magnetometer read failed: %s", esp_err_to_name(err));

    raw[0] = sample.x;
    raw[1] = sample.y;
    raw[2] = sample.z;
    lua_mag_apply_calibration(&ud->calibration, raw, corrected);

    lua_newtable(L);
    lua_mag_push_axes_table(L, corrected[0], corrected[1], corrected[2]);
    lua_setfield(L, -2, "magnetic");
    lua_mag_push_axes_table(L, raw[0], raw[1], raw[2]);
    lua_setfield(L, -2, "raw_magnetic");
    if (ud->backend->has_temperature) {
        lua_pushnumber(L, sample.temperature);
        lua_setfield(L, -2, "temperature");
    }
    lua_pushinteger(L, sample.status);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, ud->calibration.calibrated);
    lua_setfield(L, -2, "calibrated");
    return 1;
}

static int lua_mag_read_temperature(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    if (!ud->backend->has_temperature) return luaL_error(L, "temperature is not supported by this chip");
    lua_mag_sample_t sample = { 0 };
    esp_err_t err = ud->backend->read_sample(&ud->ctx, &sample);
    if (err != ESP_OK) return luaL_error(L, "magnetometer read_temperature failed: %s", esp_err_to_name(err));
    lua_pushnumber(L, sample.temperature);
    return 1;
}

static int lua_mag_read_int_status(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    uint8_t status = 0;
    esp_err_t err = ud->backend->read_status(&ud->ctx, &status);
    if (err != ESP_OK) return luaL_error(L, "magnetometer read_int_status failed: %s", esp_err_to_name(err));
    lua_pushinteger(L, status);
    return 1;
}

static int lua_mag_calibration_reset(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_calibration_reset_state(ud);
    ud->calibration.collecting = true;
    return 0;
}

static int lua_mag_calibration_add_sample(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    float sample[3];

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        if (!lua_mag_read_vec3(L, 2, sample)) {
            return luaL_error(L, "magnetometer calibration_add_sample expects {x,y,z} array");
        }
    } else {
        lua_mag_sample_t mag_sample = { 0 };
        if (ud->backend->read_sample(&ud->ctx, &mag_sample) != ESP_OK) {
            return luaL_error(L, "magnetometer calibration_add_sample read failed");
        }
        sample[0] = mag_sample.x;
        sample[1] = mag_sample.y;
        sample[2] = mag_sample.z;
    }

    for (size_t i = 0; i < 3; i++) {
        if (!isfinite(sample[i])) return luaL_error(L, "calibration sample must be finite");
    }
    lua_mag_calibration_record_sample(ud, sample);
    lua_pushinteger(L, (lua_Integer)ud->calibration.sample_count);
    return 1;
}

static int lua_mag_calibration_finish_lua(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    esp_err_t err = lua_mag_calibration_finish(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "magnetometer calibration_finish failed: %s", esp_err_to_name(err));
    }
    lua_mag_push_calibration_table(L, &ud->calibration);
    return 1;
}

static int lua_mag_calibration_get(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_push_calibration_table(L, &ud->calibration);
    return 1;
}

static int lua_mag_calibration_set(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    float hard_iron[3];
    float soft_iron[3][3];

    luaL_checktype(L, 2, LUA_TTABLE);

    lua_getfield(L, 2, "hard_iron");
    if (!lua_istable(L, -1) || !lua_mag_read_vec3(L, lua_gettop(L), hard_iron)) {
        lua_pop(L, 1);
        return luaL_error(L, "magnetometer calibration_set: missing/invalid hard_iron");
    }
    lua_pop(L, 1);

    lua_getfield(L, 2, "soft_iron");
    if (!lua_istable(L, -1) || !lua_mag_read_mat3(L, lua_gettop(L), soft_iron)) {
        lua_pop(L, 1);
        return luaL_error(L, "magnetometer calibration_set: missing/invalid soft_iron");
    }
    lua_pop(L, 1);

    memcpy(ud->calibration.hard_iron, hard_iron, sizeof(hard_iron));
    memcpy(ud->calibration.soft_iron, soft_iron, sizeof(soft_iron));
    ud->calibration.calibrated = true;
    ud->calibration.collecting = false;

    lua_mag_push_calibration_table(L, &ud->calibration);
    return 1;
}

static int lua_mag_calibration_clear(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_calibration_reset_state(ud);
    lua_mag_push_calibration_table(L, &ud->calibration);
    return 1;
}

static int option_int(lua_State *L, const char *key, int fallback, int min, int max)
{
    lua_getfield(L, 1, key);
    lua_Integer value = luaL_optinteger(L, -1, fallback);
    lua_pop(L, 1);
    if (value < min || value > max) luaL_error(L, "magnetometer.new: invalid %s", key);
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

static int lua_mag_new(lua_State *L)
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
        if (!valid) return luaL_error(L, "magnetometer.new: unsupported or mixed config fields");
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
    const lua_mag_backend_t *backend = find_backend(chip);
    if (!backend) return luaL_error(L, "magnetometer.new: device missing or chip backend not enabled");
    if (!name[0] || strlen(name) >= LUA_MAG_NAME_SIZE) return luaL_error(L, "magnetometer.new: invalid device name");

    lua_mag_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->backend = backend;
    ud->borrowed = borrowed;
    ud->int_gpio = ud->sdo_gpio = -1;
    strcpy(ud->name, name);
    luaL_getmetatable(L, LUA_MAG_METATABLE);
    lua_setmetatable(L, -2);
    int object = lua_gettop(L);
    esp_err_t err;
    if (borrowed) {
        char key[CLAW_HW_KEY_SIZE];
        const char *keys[] = {claw_hw_key_device(key, sizeof(key), name)};
        err = cap_lua_hw_claim(L, &ud->lease, "lua/magnetometer", keys, 1);
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
            (sdo_gpio >= 0 && (!backend->sdo_level_for_addr || !GPIO_IS_VALID_OUTPUT_GPIO(sdo_gpio) || sdo_gpio == int_gpio))) {
            return luaL_error(L, "magnetometer.new: unsupported address or invalid GPIO");
        }
        lua_getfield(L, 1, "bus");
        lua_pushvalue(L, -1);
        lua_setuservalue(L, object);
        int port;
        ud->ctx.master_bus = lua_driver_i2c_ref_bus(L, -1, &port, &ud->bus_owner);
        lua_pop(L, 1);
        ud->ctx.frequency = LUA_MAG_DEFAULT_FREQUENCY;
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
        err = cap_lua_hw_claim(L, &ud->lease, "lua/magnetometer", keys, count);
        if (err == ESP_OK) {
            ud->int_gpio = int_gpio;
            ud->sdo_gpio = sdo_gpio;
            err = lua_mag_configure_interrupt_pin(int_gpio);
            int level = backend->sdo_level_for_addr ? backend->sdo_level_for_addr(address) : 0;
            if (err == ESP_OK) err = lua_mag_configure_sdo_pin(sdo_gpio, level);
            if (err == ESP_OK) err = backend->create(&ud->ctx, address);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "magnetometer '%s' open failed: %s", name, esp_err_to_name(err));
        esp_err_t cleanup = lua_mag_release(ud);
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "Open cleanup failed: %s", esp_err_to_name(cleanup));
        return luaL_error(L, "magnetometer.new failed: %s", esp_err_to_name(err));
    }
    lua_mag_calibration_reset_state(ud);
    ud->ready = true;
    return 1;
}

int luaopen_magnetometer(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"read", lua_mag_read}, {"read_temperature", lua_mag_read_temperature},
        {"read_int_status", lua_mag_read_int_status}, {"chip_id", lua_mag_chip_id},
        {"calibration_reset", lua_mag_calibration_reset}, {"calibration_add_sample", lua_mag_calibration_add_sample},
        {"calibration_finish", lua_mag_calibration_finish_lua}, {"calibration_get", lua_mag_calibration_get},
        {"calibration_set", lua_mag_calibration_set}, {"calibration_clear", lua_mag_calibration_clear},
        {"name", lua_mag_name}, {"close", lua_mag_close}, {"__gc", lua_mag_gc}, {NULL, NULL},
    };
    if (luaL_newmetatable(L, LUA_MAG_METATABLE)) {
        luaL_setfuncs(L, methods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, lua_mag_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_magnetometer_register(void)
{
    return cap_lua_register_module("magnetometer", luaopen_magnetometer);
}
