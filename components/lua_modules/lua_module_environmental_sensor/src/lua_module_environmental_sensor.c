/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_environmental_sensor.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
#include "bme69x.h"
#include "bme69x_defs.h"
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
#include "dht.h"
#include "driver/gpio.h"
#endif
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_MODULE_ENVIRONMENTAL_SENSOR_NAME      "environmental_sensor"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT  "dht"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_BME690 "bme690"

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
#define LUA_MODULE_BME690_METATABLE        "environmental_sensor.device"
#define LUA_MODULE_BME690_DEFAULT_NAME     "environmental_sensor"
#define LUA_MODULE_BME690_LEGACY_NAME      "bme690_sensor"
#define LUA_MODULE_BME690_MAX_NAME_LEN     64
#define LUA_MODULE_BME690_DEFAULT_FREQ_HZ  400000
#define LUA_MODULE_BME690_DEFAULT_HEAT_C   300
#define LUA_MODULE_BME690_DEFAULT_HEAT_MS  100

typedef struct {
    i2c_bus_handle_t i2c_bus_handle;
    i2c_bus_device_handle_t i2c_dev_handle;
    struct bme69x_dev sensor_handle;
    char peripheral_name[LUA_MODULE_BME690_MAX_NAME_LEN];
    bool peripheral_ref_held;
    bool sensor_initialized;
    uint8_t i2c_addr;
    uint16_t heatr_temp;
    uint16_t heatr_dur;
} lua_module_bme690_handle_t;

typedef struct {
    lua_module_bme690_handle_t *handle;
    char device_name[LUA_MODULE_BME690_MAX_NAME_LEN];
} lua_module_bme690_ud_t;

/*
 * Local mirror of the auto-generated `dev_custom_environmental_sensor_config_t`
 * struct (or whatever name the board manager emits for this device).
 *
 * IMPORTANT: This MUST be byte-for-byte identical to the auto-generated
 * struct. `lua_bme690_resolve_board_cfg()` cross-checks the size reported
 * by the board manager descriptor against `sizeof(lua_bme690_board_cfg_t)`
 * and refuses to use the config if they differ -- otherwise a YAML schema
 * mismatch would silently misinterpret bytes (e.g. read the wrong field
 * as `i2c_addr` or `frequency`).
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_bme690_board_cfg_t;

typedef struct {
    char peripheral_name[LUA_MODULE_BME690_MAX_NAME_LEN];
    int i2c_addr;
    int frequency;
    bool has_peripheral;
    bool has_i2c_addr;
    bool has_frequency;
    uint16_t heatr_temp;
    uint16_t heatr_dur;
} lua_bme690_resolved_cfg_t;

static const char *TAG = "lua_module_bme690";

static void lua_module_bme690_destroy_handle(lua_module_bme690_handle_t *handle);

static esp_err_t lua_module_bme690_open_i2c_bus(const char *peripheral_name,
                                                int frequency,
                                                i2c_bus_handle_t *i2c_bus_handle,
                                                bool *peripheral_ref_held)
{
    i2c_master_bus_handle_t i2c_master_handle = NULL;
    i2c_master_bus_config_t *i2c_master_cfg = NULL;

    *peripheral_ref_held = false;

    ESP_RETURN_ON_ERROR(esp_board_periph_ref_handle(peripheral_name, (void **)&i2c_master_handle),
                        TAG, "Failed to reference board I2C bus '%s'", peripheral_name);
    *peripheral_ref_held = true;

    esp_err_t err = esp_board_periph_get_config(peripheral_name, (void **)&i2c_master_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get board I2C config '%s': %s", peripheral_name, esp_err_to_name(err));
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return err;
    }

    const i2c_config_t i2c_bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_master_cfg->sda_io_num,
        .scl_io_num = i2c_master_cfg->scl_io_num,
        .sda_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .scl_pullup_en = i2c_master_cfg->flags.enable_internal_pullup,
        .master.clk_speed = (uint32_t)frequency,
        .clk_flags = 0,
    };

    (void)i2c_master_handle;
    *i2c_bus_handle = i2c_bus_create(i2c_master_cfg->i2c_port, &i2c_bus_cfg);
    if (*i2c_bus_handle == NULL) {
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_bme690_select_addr(lua_module_bme690_handle_t *handle, uint8_t i2c_addr)
{
    if (handle->i2c_dev_handle != NULL && handle->i2c_addr == i2c_addr) {
        return ESP_OK;
    }

    if (handle->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->i2c_dev_handle);
        handle->i2c_dev_handle = NULL;
    }

    handle->i2c_dev_handle = i2c_bus_device_create(handle->i2c_bus_handle, i2c_addr, 0);
    if (handle->i2c_dev_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create environmental sensor I2C device for address 0x%02x", i2c_addr);
        return ESP_FAIL;
    }

    handle->i2c_addr = i2c_addr;
    return ESP_OK;
}

static esp_err_t lua_module_bme690_probe_chip(lua_module_bme690_handle_t *handle)
{
    uint8_t chip_id = 0;
    esp_err_t err = i2c_bus_read_bytes(handle->i2c_dev_handle, BME69X_REG_CHIP_ID, 1, &chip_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BME690 chip ID at 0x%02x: %s",
                 handle->i2c_addr, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Environmental sensor probe at 0x%02x -> chip_id=0x%02x",
             handle->i2c_addr, chip_id);
    if (chip_id != BME69X_CHIP_ID) {
        ESP_LOGE(TAG,
                 "Unexpected environmental sensor chip ID 0x%02x at 0x%02x, expected 0x%02x. "
                 "Check whether the BME690 sub-board is inserted.",
                 chip_id, handle->i2c_addr, BME69X_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static BME69X_INTF_RET_TYPE lua_module_bme690_i2c_read(uint8_t reg_addr,
                                                       uint8_t *reg_data,
                                                       uint32_t len,
                                                       void *intf_ptr)
{
    lua_module_bme690_handle_t *handle = (lua_module_bme690_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev_handle == NULL) {
        return BME69X_E_COM_FAIL;
    }

    esp_err_t err = i2c_bus_read_bytes(handle->i2c_dev_handle, reg_addr, (size_t)len, reg_data);
    return (err == ESP_OK) ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}

static BME69X_INTF_RET_TYPE lua_module_bme690_i2c_write(uint8_t reg_addr,
                                                        const uint8_t *reg_data,
                                                        uint32_t len,
                                                        void *intf_ptr)
{
    lua_module_bme690_handle_t *handle = (lua_module_bme690_handle_t *)intf_ptr;
    if (handle == NULL || handle->i2c_dev_handle == NULL) {
        return BME69X_E_COM_FAIL;
    }

    esp_err_t err = i2c_bus_write_bytes(handle->i2c_dev_handle, reg_addr, (size_t)len, reg_data);
    return (err == ESP_OK) ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}

static void lua_module_bme690_delay_us(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_us < 1000) {
        esp_rom_delay_us(period_us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    }
}

static esp_err_t lua_module_bme690_apply_default_runtime_config(lua_module_bme690_handle_t *handle)
{
    struct bme69x_conf conf = {
        .filter = BME69X_FILTER_OFF,
        .odr = BME69X_ODR_NONE,
        .os_hum = BME69X_OS_16X,
        .os_pres = BME69X_OS_16X,
        .os_temp = BME69X_OS_16X,
    };
    struct bme69x_heatr_conf heatr_conf = {
        .enable = BME69X_ENABLE,
        .heatr_temp = handle->heatr_temp,
        .heatr_dur = handle->heatr_dur,
    };

    int8_t rslt = bme69x_set_conf(&conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Failed to configure BME690 oversampling: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "Failed to configure BME690 heater: %d", rslt);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_bme690_read_sample(lua_module_bme690_handle_t *handle,
                                               struct bme69x_data *data)
{
    struct bme69x_conf conf = {
        .filter = BME69X_FILTER_OFF,
        .odr = BME69X_ODR_NONE,
        .os_hum = BME69X_OS_16X,
        .os_pres = BME69X_OS_16X,
        .os_temp = BME69X_OS_16X,
    };
    struct bme69x_heatr_conf heatr_conf = {
        .enable = BME69X_ENABLE,
        .heatr_temp = handle->heatr_temp,
        .heatr_dur = handle->heatr_dur,
    };
    uint8_t n_data = 0;

    int8_t rslt = bme69x_set_conf(&conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr_conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_heatr_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_op_mode failed: %d", rslt);
        return ESP_FAIL;
    }

    uint32_t delay_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &handle->sensor_handle) +
                        ((uint32_t)heatr_conf.heatr_dur * 1000U);
    handle->sensor_handle.delay_us(delay_us, handle->sensor_handle.intf_ptr);

    rslt = bme69x_get_data(BME69X_FORCED_MODE, data, &n_data, &handle->sensor_handle);
    if (rslt != BME69X_OK || n_data == 0) {
        ESP_LOGW(TAG, "bme69x_get_data failed or empty: rslt=%d n_data=%u", rslt, n_data);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t lua_module_bme690_create_handle(const lua_bme690_resolved_cfg_t *cfg,
                                                 lua_module_bme690_handle_t **out_handle)
{
    lua_module_bme690_handle_t *handle = calloc(1, sizeof(lua_module_bme690_handle_t));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->heatr_temp = cfg->heatr_temp;
    handle->heatr_dur = cfg->heatr_dur;

    esp_err_t err = lua_module_bme690_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                                                   &handle->i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }

    err = lua_module_bme690_select_addr(handle, (uint8_t)cfg->i2c_addr);
    if (err != ESP_OK) {
        lua_module_bme690_destroy_handle(handle);
        return err;
    }

    err = lua_module_bme690_probe_chip(handle);
    if (err != ESP_OK) {
        lua_module_bme690_destroy_handle(handle);
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    memset(&handle->sensor_handle, 0, sizeof(handle->sensor_handle));
    handle->sensor_handle.read = lua_module_bme690_i2c_read;
    handle->sensor_handle.write = lua_module_bme690_i2c_write;
    handle->sensor_handle.delay_us = lua_module_bme690_delay_us;
    handle->sensor_handle.intf = BME69X_I2C_INTF;
    handle->sensor_handle.intf_ptr = handle;

    int8_t rslt = BME69X_OK;
    rslt = bme69x_init(&handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_init failed: %d", rslt);
        lua_module_bme690_destroy_handle(handle);
        return (rslt == BME69X_E_DEV_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    err = lua_module_bme690_apply_default_runtime_config(handle);
    if (err != ESP_OK) {
        lua_module_bme690_destroy_handle(handle);
        return err;
    }

    handle->sensor_initialized = true;
    *out_handle = handle;
    ESP_LOGI(TAG, "BME690 initialized on %s, addr 0x%02x, freq %d Hz",
             cfg->peripheral_name, cfg->i2c_addr, cfg->frequency);
    return ESP_OK;
}

static void lua_module_bme690_destroy_handle(lua_module_bme690_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->i2c_dev_handle);
        handle->i2c_dev_handle = NULL;
    }
    if (handle->peripheral_ref_held && handle->peripheral_name[0] != '\0') {
        esp_board_periph_unref_handle(handle->peripheral_name);
    }
    free(handle);
}

static lua_module_bme690_ud_t *lua_module_bme690_get_ud(lua_State *L, int idx)
{
    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_BME690_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized) {
        luaL_error(L, "environmental_sensor: invalid or closed handle");
    }
    return ud;
}

static int lua_module_bme690_close_impl(lua_State *L, lua_module_bme690_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_module_bme690_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_module_bme690_gc(lua_State *L)
{
    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)luaL_testudata(L, 1, LUA_MODULE_BME690_METATABLE);
    if (ud && ud->handle) {
        return lua_module_bme690_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_bme690_close(lua_State *L)
{
    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_BME690_METATABLE);
    if (ud->handle) {
        return lua_module_bme690_close_impl(L, ud);
    }
    return 0;
}

static int lua_module_bme690_name(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_module_bme690_chip_id(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushinteger(L, ud->handle->sensor_handle.chip_id);
    return 1;
}

static int lua_module_bme690_variant_id(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)ud->handle->sensor_handle.variant_id);
    return 1;
}

static int lua_module_bme690_read(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read failed");
    }

    lua_newtable(L);
    lua_pushnumber(L, data.temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, data.pressure);
    lua_setfield(L, -2, "pressure");
    lua_pushnumber(L, data.humidity);
    lua_setfield(L, -2, "humidity");
    lua_pushnumber(L, data.gas_resistance);
    lua_setfield(L, -2, "gas_resistance");
    lua_pushinteger(L, data.status);
    lua_setfield(L, -2, "status");
    lua_pushinteger(L, data.gas_index);
    lua_setfield(L, -2, "gas_index");
    lua_pushinteger(L, data.meas_index);
    lua_setfield(L, -2, "meas_index");
    return 1;
}

static int lua_module_bme690_read_temperature(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_temperature failed");
    }

    lua_pushnumber(L, data.temperature);
    return 1;
}

static int lua_module_bme690_read_pressure(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_pressure failed");
    }

    lua_pushnumber(L, data.pressure);
    return 1;
}

static int lua_module_bme690_read_humidity(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_humidity failed");
    }

    lua_pushnumber(L, data.humidity);
    return 1;
}

static int lua_module_bme690_read_gas(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud->handle, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_gas failed");
    }

    lua_pushnumber(L, data.gas_resistance);
    return 1;
}

/*
 * Walk the board manager descriptor list and verify that the auto-generated
 * config for `device_name` has the layout `lua_bme690_board_cfg_t` expects.
 * Returns ESP_ERR_NOT_FOUND if the board doesn't declare the device, and
 * ESP_ERR_INVALID_SIZE if the schema diverges from this module's mirror.
 */
static esp_err_t lua_bme690_resolve_board_cfg(const char *device_name,
                                              const lua_bme690_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *desc = g_esp_board_devices;
    while (desc != NULL && desc->name != NULL) {
        if (strcmp(desc->name, device_name) == 0) {
            if (desc->cfg == NULL) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(lua_bme690_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u differs from expected %u; "
                         "board_devices.yaml schema is out of sync with lua_bme690_board_cfg_t. "
                         "Every field listed in lua_bme690_board_cfg_t MUST be present in YAML "
                         "(use -1 for unused GPIOs).",
                         device_name,
                         (unsigned)desc->cfg_size,
                         (unsigned)sizeof(lua_bme690_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const lua_bme690_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t lua_module_bme690_load_board_defaults(const char *device_name,
                                                       lua_bme690_resolved_cfg_t *out)
{
    const lua_bme690_board_cfg_t *board = NULL;
    esp_err_t err = lua_bme690_resolve_board_cfg(device_name, &board);
    if (err != ESP_OK) {
        return err;
    }

    if (board->chip != NULL && strcmp(board->chip, LUA_MODULE_ENVIRONMENTAL_SENSOR_SELECTED_CHIP_NAME) != 0) {
        ESP_LOGW(TAG, "Board device '%s' chip='%s' does not match %s backend",
                 device_name, board->chip, LUA_MODULE_ENVIRONMENTAL_SENSOR_SELECTED_CHIP_NAME);
    }

    if (board->peripheral_name != NULL && board->peripheral_name[0] != '\0') {
        snprintf(out->peripheral_name, sizeof(out->peripheral_name), "%s", board->peripheral_name);
        out->has_peripheral = true;
    }
    if (board->i2c_addr != 0) {
        out->i2c_addr = board->i2c_addr;
        out->has_i2c_addr = true;
    }
    if (board->frequency > 0) {
        out->frequency = board->frequency;
        out->has_frequency = true;
    }

    return ESP_OK;
}

static void lua_module_bme690_apply_lua_overrides(lua_State *L, int opts_idx,
                                                  lua_bme690_resolved_cfg_t *cfg)
{
    if (opts_idx == 0 || lua_type(L, opts_idx) != LUA_TTABLE) {
        return;
    }

    lua_getfield(L, opts_idx, "peripheral");
    if (lua_isstring(L, -1)) {
        const char *p = lua_tostring(L, -1);
        snprintf(cfg->peripheral_name, sizeof(cfg->peripheral_name), "%s", p);
        cfg->has_peripheral = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "i2c_addr");
    if (lua_isnumber(L, -1)) {
        cfg->i2c_addr = (int)lua_tointeger(L, -1);
        cfg->has_i2c_addr = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "frequency");
    if (lua_isnumber(L, -1)) {
        cfg->frequency = (int)lua_tointeger(L, -1);
        cfg->has_frequency = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "heater_temp");
    if (lua_isnumber(L, -1)) {
        cfg->heatr_temp = (uint16_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "heater_duration");
    if (lua_isnumber(L, -1)) {
        cfg->heatr_dur = (uint16_t)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
}

static int lua_module_bme690_new(lua_State *L)
{
    const char *device_name = LUA_MODULE_BME690_DEFAULT_NAME;
    int opts_idx = 0;

    if (lua_isstring(L, 1)) {
        device_name = lua_tostring(L, 1);
        if (lua_istable(L, 2)) {
            opts_idx = 2;
        }
    } else if (lua_istable(L, 1)) {
        opts_idx = 1;
        lua_getfield(L, 1, "device");
        if (lua_isstring(L, -1)) {
            device_name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (strlen(device_name) >= LUA_MODULE_BME690_MAX_NAME_LEN) {
        return luaL_error(L, "environmental_sensor device name too long");
    }

    lua_bme690_resolved_cfg_t cfg = {
        .heatr_temp = LUA_MODULE_BME690_DEFAULT_HEAT_C,
        .heatr_dur = LUA_MODULE_BME690_DEFAULT_HEAT_MS,
    };

    esp_err_t err = lua_module_bme690_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;
    if (err == ESP_ERR_INVALID_SIZE) {
        return luaL_error(L,
                          "environmental_sensor.new: board device '%s' config schema mismatch "
                          "(see error log above for details)", device_name);
    }
    if (err != ESP_OK && strcmp(device_name, LUA_MODULE_BME690_DEFAULT_NAME) == 0) {
        esp_err_t legacy_err = lua_module_bme690_load_board_defaults(LUA_MODULE_BME690_LEGACY_NAME, &cfg);
        if (legacy_err == ESP_OK) {
            opened_device_name = LUA_MODULE_BME690_LEGACY_NAME;
            ESP_LOGW(TAG, "Default device '%s' not declared, using legacy '%s'",
                     LUA_MODULE_BME690_DEFAULT_NAME, LUA_MODULE_BME690_LEGACY_NAME);
        } else if (legacy_err == ESP_ERR_INVALID_SIZE) {
            return luaL_error(L,
                              "environmental_sensor.new: legacy board device '%s' config schema mismatch",
                              LUA_MODULE_BME690_LEGACY_NAME);
        }
    }

    lua_module_bme690_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "environmental_sensor.new: missing 'peripheral' (board declares no '%s', "
                              "and no override given)", device_name);
    }
    if (!cfg.has_i2c_addr) {
        cfg.i2c_addr = BME69X_I2C_ADDR_LOW;
        cfg.has_i2c_addr = true;
    }
    if (!cfg.has_frequency) {
        cfg.frequency = LUA_MODULE_BME690_DEFAULT_FREQ_HZ;
        cfg.has_frequency = true;
    }
    if (cfg.i2c_addr != BME69X_I2C_ADDR_LOW && cfg.i2c_addr != BME69X_I2C_ADDR_HIGH) {
        return luaL_error(L, "environmental_sensor.new: unsupported BME690 I2C address 0x%d (expected 0x76 or 0x77)",
                          cfg.i2c_addr);
    }

    lua_module_bme690_handle_t *handle = NULL;
    err = lua_module_bme690_create_handle(&cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "environmental_sensor.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_module_bme690_ud_t *ud =
        (lua_module_bme690_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);

    luaL_getmetatable(L, LUA_MODULE_BME690_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}
#endif

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
#define LUA_MODULE_DHT_METATABLE           "environmental_sensor.dht_device"
#define LUA_MODULE_DHT_DEFAULT_TYPE        DHT_TYPE_DHT11
#define LUA_MODULE_DHT_PRE_READ_DELAY_US   (200 * 1000)

typedef struct {
    gpio_num_t pin;
    dht_sensor_type_t sensor_type;
    bool closed;
} lua_module_dht_ud_t;

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

static lua_module_dht_ud_t *lua_module_dht_get_ud(lua_State *L, int idx)
{
    lua_module_dht_ud_t *ud =
        (lua_module_dht_ud_t *)luaL_checkudata(L, idx, LUA_MODULE_DHT_METATABLE);
    if (ud == NULL || ud->closed) {
        luaL_error(L, "environmental_sensor: invalid or closed dht handle");
    }
    return ud;
}

static esp_err_t lua_module_dht_read_float(dht_sensor_type_t sensor_type, gpio_num_t pin,
                                           float *temperature, float *humidity)
{
    esp_rom_delay_us(LUA_MODULE_DHT_PRE_READ_DELAY_US);
    return dht_read_float_data(sensor_type, pin, humidity, temperature);
}

static int lua_module_dht_close(lua_State *L)
{
    lua_module_dht_ud_t *ud =
        (lua_module_dht_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_DHT_METATABLE);
    ud->closed = true;
    return 0;
}

static int lua_module_dht_gc(lua_State *L)
{
    lua_module_dht_ud_t *ud =
        (lua_module_dht_ud_t *)luaL_testudata(L, 1, LUA_MODULE_DHT_METATABLE);
    if (ud != NULL) {
        ud->closed = true;
    }
    return 0;
}

static int lua_module_dht_name(lua_State *L)
{
    lua_module_dht_get_ud(L, 1);
    lua_pushstring(L, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT);
    return 1;
}

static int lua_module_dht_read(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = lua_module_dht_read_float(ud->sensor_type, ud->pin, &temperature, &humidity);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushnumber(L, temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushnumber(L, humidity);
    lua_setfield(L, -2, "humidity");
    return 1;
}

static int lua_module_dht_read_temperature(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = lua_module_dht_read_float(ud->sensor_type, ud->pin, &temperature, &humidity);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read_temperature failed: %s", esp_err_to_name(err));
    }

    lua_pushnumber(L, temperature);
    return 1;
}

static int lua_module_dht_read_humidity(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    float humidity = 0;
    float temperature = 0;

    esp_err_t err = lua_module_dht_read_float(ud->sensor_type, ud->pin, &temperature, &humidity);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read_humidity failed: %s", esp_err_to_name(err));
    }

    lua_pushnumber(L, humidity);
    return 1;
}

static int lua_module_dht_read_raw_method(lua_State *L)
{
    lua_module_dht_ud_t *ud = lua_module_dht_get_ud(L, 1);
    int16_t humidity = 0;
    int16_t temperature = 0;

    esp_rom_delay_us(LUA_MODULE_DHT_PRE_READ_DELAY_US);
    esp_err_t err = dht_read_data(ud->sensor_type, ud->pin, &humidity, &temperature);
    if (err != ESP_OK) {
        return luaL_error(L, "environmental_sensor dht read_raw failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, temperature);
    lua_pushinteger(L, humidity);
    return 2;
}

static int lua_module_dht_new(lua_State *L)
{
    int opts_idx = 0;

    if (lua_istable(L, 1)) {
        opts_idx = 1;
    } else if (lua_istable(L, 2)) {
        opts_idx = 2;
    }

    if (opts_idx == 0) {
        return luaL_error(L, "environmental_sensor.new({ type = \"dht\", pin = <gpio> ... }) expects an options table");
    }

    lua_getfield(L, opts_idx, "pin");
    gpio_num_t pin = (gpio_num_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "sensor_type");
    const char *sensor_type_str = lua_isnoneornil(L, -1) ? NULL : luaL_checkstring(L, -1);
    dht_sensor_type_t sensor_type = lua_module_dht_sensor_type_from_string(sensor_type_str);
    lua_pop(L, 1);

    if ((int)sensor_type < 0) {
        return luaL_error(L, "environmental_sensor.new: invalid dht sensor_type");
    }

    lua_module_dht_ud_t *ud = (lua_module_dht_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->pin = pin;
    ud->sensor_type = sensor_type;

    luaL_getmetatable(L, LUA_MODULE_DHT_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static void lua_module_dht_create_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_DHT_METATABLE)) {
        lua_pushcfunction(L, lua_module_dht_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_dht_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_dht_read_raw_method);
        lua_setfield(L, -2, "read_raw");
        lua_pushcfunction(L, lua_module_dht_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_dht_read_humidity);
        lua_setfield(L, -2, "read_humidity");
        lua_pushcfunction(L, lua_module_dht_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_dht_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}
#endif

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
static void lua_module_environmental_sensor_create_bme690_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_BME690_METATABLE)) {
        lua_pushcfunction(L, lua_module_bme690_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_bme690_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_bme690_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_module_bme690_read_pressure);
        lua_setfield(L, -2, "read_pressure");
        lua_pushcfunction(L, lua_module_bme690_read_humidity);
        lua_setfield(L, -2, "read_humidity");
        lua_pushcfunction(L, lua_module_bme690_read_gas);
        lua_setfield(L, -2, "read_gas");
        lua_pushcfunction(L, lua_module_bme690_chip_id);
        lua_setfield(L, -2, "chip_id");
        lua_pushcfunction(L, lua_module_bme690_variant_id);
        lua_setfield(L, -2, "variant_id");
        lua_pushcfunction(L, lua_module_bme690_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_module_bme690_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
}
#endif

static bool lua_module_environmental_sensor_table_has_field(lua_State *L, int idx, const char *field)
{
    bool has_field = false;

    if (!lua_istable(L, idx)) {
        return false;
    }

    lua_getfield(L, idx, field);
    has_field = !lua_isnoneornil(L, -1);
    lua_pop(L, 1);
    return has_field;
}

static int lua_module_environmental_sensor_new(lua_State *L)
{
    const char *backend_type = NULL;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "type");
        if (lua_isstring(L, -1)) {
            backend_type = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (backend_type == NULL && lua_istable(L, 1) &&
        lua_module_environmental_sensor_table_has_field(L, 1, "pin")) {
        backend_type = LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT;
    }

    if (backend_type == NULL) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
        return lua_module_bme690_new(L);
#elif CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
        return lua_module_dht_new(L);
#else
        return luaL_error(L, "environmental_sensor has no enabled backend");
#endif
    }

    if (strcmp(backend_type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_BME690) == 0) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
        return lua_module_bme690_new(L);
#else
        return luaL_error(L, "environmental_sensor backend '%s' is not enabled in menuconfig", backend_type);
#endif
    }

    if (strcmp(backend_type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT) == 0) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
        return lua_module_dht_new(L);
#else
        return luaL_error(L, "environmental_sensor backend '%s' is not enabled in menuconfig", backend_type);
#endif
    }

    return luaL_error(L, "environmental_sensor.new: unsupported type '%s'", backend_type);
}

int luaopen_environmental_sensor(lua_State *L)
{
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
    lua_module_environmental_sensor_create_bme690_metatable(L);
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
    lua_module_dht_create_metatable(L);
#endif

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_environmental_sensor_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_environmental_sensor_register(void)
{
    return cap_lua_register_module(LUA_MODULE_ENVIRONMENTAL_SENSOR_NAME,
                                   luaopen_environmental_sensor);
}
