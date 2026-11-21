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
#include "cap_lua_hw.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
#include "bme69x.h"
#include "lua_driver_i2c.h"
#endif
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
#include "dht.h"
#include "driver/gpio.h"
#endif

#define LUA_MODULE_ENVIRONMENTAL_SENSOR_NAME "environmental_sensor"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT "dht"
#define LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_BME690 "bme690"

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690 || CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
static const char *TAG = "lua_env_sensor";

static int option_int(lua_State *L, const char *name, int def, int min, int max)
{
    lua_getfield(L, 1, name);
    lua_Integer value = lua_isnil(L, -1) ? def : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (value < min || value > max) luaL_error(L, "environmental_sensor: invalid %s", name);
    return (int)value;
}

#endif

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
#define LUA_MODULE_BME690_METATABLE "environmental_sensor.device"
#define LUA_MODULE_BME690_DEFAULT_FREQ_HZ 400000
#define LUA_MODULE_BME690_DEFAULT_HEAT_C 300
#define LUA_MODULE_BME690_DEFAULT_HEAT_MS 100
#define LUA_MODULE_BME690_I2C_TIMEOUT_MS 1000

typedef struct {
    i2c_master_dev_handle_t i2c_dev_handle;
    lua_driver_i2c_bus_handle_t bus_owner;
    cap_lua_hw_lease_t lease;
    struct bme69x_dev sensor_handle;
    uint16_t heatr_temp;
    uint16_t heatr_dur;
    bool sensor_initialized;
} lua_module_bme690_ud_t;

static esp_err_t lua_module_bme690_release(lua_module_bme690_ud_t *ud)
{
    ud->sensor_initialized = false;
    if (ud->i2c_dev_handle) {
        esp_err_t err = i2c_master_bus_rm_device(ud->i2c_dev_handle);
        if (err != ESP_OK) return err;
        ud->i2c_dev_handle = NULL;
    }
    lua_driver_i2c_unref_bus(ud->bus_owner);
    ud->bus_owner = NULL;
    return cap_lua_hw_release(&ud->lease);
}

static BME69X_INTF_RET_TYPE lua_module_bme690_i2c_read(uint8_t reg, uint8_t *data, uint32_t len, void *arg)
{
    lua_module_bme690_ud_t *ud = arg;
    esp_err_t err = i2c_master_transmit_receive(ud->i2c_dev_handle, &reg, 1, data, len, LUA_MODULE_BME690_I2C_TIMEOUT_MS);
    return err == ESP_OK ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}

static BME69X_INTF_RET_TYPE lua_module_bme690_i2c_write(uint8_t reg, const uint8_t *data, uint32_t len, void *arg)
{
    lua_module_bme690_ud_t *ud = arg;
    if (len > UINT8_MAX) return BME69X_E_COM_FAIL;
    uint8_t *buffer = malloc(len + 1);
    if (!buffer) return BME69X_E_COM_FAIL;
    buffer[0] = reg;
    memcpy(buffer + 1, data, len);
    esp_err_t err = i2c_master_transmit(ud->i2c_dev_handle, buffer, len + 1, LUA_MODULE_BME690_I2C_TIMEOUT_MS);
    free(buffer);
    return err == ESP_OK ? BME69X_INTF_RET_SUCCESS : BME69X_E_COM_FAIL;
}

static void lua_module_bme690_delay_us(uint32_t us, void *arg)
{
    (void)arg;
    if (us < 1000) esp_rom_delay_us(us);
    else vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000) + 1);
}

static esp_err_t lua_module_bme690_apply_default_runtime_config(lua_module_bme690_ud_t *handle)
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

static esp_err_t lua_module_bme690_read_sample(lua_module_bme690_ud_t *handle,
                                               struct bme69x_data *data)
{
    struct bme69x_conf conf = {0};
    uint8_t n_data = 0;
    int8_t rslt = bme69x_get_conf(&conf, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_get_conf failed: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &handle->sensor_handle);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_op_mode failed: %d", rslt);
        return ESP_FAIL;
    }

    uint32_t delay_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &handle->sensor_handle) +
                        ((uint32_t)handle->heatr_dur * 1000U);
    handle->sensor_handle.delay_us(delay_us, handle->sensor_handle.intf_ptr);

    rslt = bme69x_get_data(BME69X_FORCED_MODE, data, &n_data, &handle->sensor_handle);
    if (rslt != BME69X_OK || n_data == 0) {
        ESP_LOGW(TAG, "bme69x_get_data failed or empty: rslt=%d n_data=%u", rslt, n_data);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static lua_module_bme690_ud_t *lua_module_bme690_get_ud(lua_State *L, int index)
{
    lua_module_bme690_ud_t *ud = luaL_checkudata(L, index, LUA_MODULE_BME690_METATABLE);
    luaL_argcheck(L, ud->sensor_initialized, index, "invalid or closed environmental sensor");
    return ud;
}

static int lua_module_bme690_close(lua_State *L)
{
    lua_module_bme690_ud_t *ud = luaL_checkudata(L, 1, LUA_MODULE_BME690_METATABLE);
    esp_err_t err = lua_module_bme690_release(ud);
    if (err != ESP_OK) return luaL_error(L, "environmental_sensor close failed: %s", esp_err_to_name(err));
    lua_pushnil(L);
    lua_setuservalue(L, 1);
    return 0;
}

static int lua_module_bme690_gc(lua_State *L)
{
    lua_module_bme690_ud_t *ud = luaL_checkudata(L, 1, LUA_MODULE_BME690_METATABLE);
    esp_err_t err = lua_module_bme690_release(ud);
    if (err != ESP_OK) ESP_LOGE(TAG, "BME690 cleanup failed: %s", esp_err_to_name(err));
    return 0;
}

static int lua_module_bme690_name(lua_State *L)
{
    lua_module_bme690_get_ud(L, 1);
    lua_pushliteral(L, "bme690");
    return 1;
}

static int lua_module_bme690_chip_id(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushinteger(L, ud->sensor_handle.chip_id);
    return 1;
}

static int lua_module_bme690_variant_id(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    lua_pushinteger(L, (lua_Integer)ud->sensor_handle.variant_id);
    return 1;
}

static int lua_module_bme690_read(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud, &data) != ESP_OK) {
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

    if (lua_module_bme690_read_sample(ud, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_temperature failed");
    }

    lua_pushnumber(L, data.temperature);
    return 1;
}

static int lua_module_bme690_read_pressure(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_pressure failed");
    }

    lua_pushnumber(L, data.pressure);
    return 1;
}

static int lua_module_bme690_read_humidity(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_humidity failed");
    }

    lua_pushnumber(L, data.humidity);
    return 1;
}

static int lua_module_bme690_read_gas(lua_State *L)
{
    lua_module_bme690_ud_t *ud = lua_module_bme690_get_ud(L, 1);
    struct bme69x_data data = { 0 };

    if (lua_module_bme690_read_sample(ud, &data) != ESP_OK) {
        return luaL_error(L, "environmental_sensor read_gas failed");
    }

    lua_pushnumber(L, data.gas_resistance);
    return 1;
}

static int lua_module_bme690_new(lua_State *L)
{
    int address = option_int(L, "address", BME69X_I2C_ADDR_LOW, BME69X_I2C_ADDR_LOW, BME69X_I2C_ADDR_HIGH);
    int frequency = option_int(L, "frequency", LUA_MODULE_BME690_DEFAULT_FREQ_HZ, 1, LUA_MODULE_BME690_DEFAULT_FREQ_HZ);
    int temperature = option_int(L, "heater_temp", LUA_MODULE_BME690_DEFAULT_HEAT_C, 0, 400);
    int duration = option_int(L, "heater_duration", LUA_MODULE_BME690_DEFAULT_HEAT_MS, 0, 4032);
    lua_getfield(L, 1, "bus");
    int bus_index = lua_gettop(L);
    lua_module_bme690_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    luaL_setmetatable(L, LUA_MODULE_BME690_METATABLE);
    /* Keep the bus alive until the native device has been removed. */
    lua_pushvalue(L, bus_index);
    lua_setuservalue(L, -2);
    int port;
    i2c_master_bus_handle_t bus = lua_driver_i2c_ref_bus(L, bus_index, &port, &ud->bus_owner);
    char key[CLAW_HW_KEY_SIZE];
    const char *keys[] = {claw_hw_key_i2c_addr(key, sizeof(key), port, address)};
    esp_err_t err = cap_lua_hw_claim(L, &ud->lease, "lua/environmental_sensor", keys, 1);
    if (err == ESP_OK) {
        i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = frequency,
        };
        err = i2c_master_bus_add_device(bus, &config, &ud->i2c_dev_handle);
    }
    if (err == ESP_OK) {
        ud->heatr_temp = temperature;
        ud->heatr_dur = duration;
        ud->sensor_handle = (struct bme69x_dev) {
            .read = lua_module_bme690_i2c_read,
            .write = lua_module_bme690_i2c_write,
            .delay_us = lua_module_bme690_delay_us,
            .intf = BME69X_I2C_INTF,
            .intf_ptr = ud,
            .amb_temp = CONFIG_BME690_AMBIENT_TEMP,
        };
        int8_t result = bme69x_init(&ud->sensor_handle);
        if (result != BME69X_OK || ud->sensor_handle.chip_id != BME69X_CHIP_ID) err = ESP_ERR_NOT_FOUND;
        else err = lua_module_bme690_apply_default_runtime_config(ud);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BME690 init failed at 0x%02x: %s", address, esp_err_to_name(err));
        esp_err_t cleanup = lua_module_bme690_release(ud);
        if (cleanup != ESP_OK) ESP_LOGE(TAG, "BME690 cleanup failed: %s", esp_err_to_name(cleanup));
        return luaL_error(L, "environmental_sensor.new failed: %s", esp_err_to_name(err));
    }
    ud->sensor_initialized = true;
    return 1;
}
#endif

#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
#define LUA_MODULE_DHT_METATABLE           "environmental_sensor.dht_device"
#define LUA_MODULE_DHT_PRE_READ_DELAY_US   (200 * 1000)

typedef struct {
    gpio_num_t pin;
    dht_sensor_type_t sensor_type;
    bool closed;
    cap_lua_hw_lease_t lease;
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
    vTaskDelay(pdMS_TO_TICKS(LUA_MODULE_DHT_PRE_READ_DELAY_US / 1000) + 1);
    return dht_read_float_data(sensor_type, pin, humidity, temperature);
}

static esp_err_t lua_module_dht_release(lua_module_dht_ud_t *ud)
{
    ud->closed = true;
    if (ud->lease.id) {
        esp_err_t err = gpio_reset_pin(ud->pin);
        if (err != ESP_OK) return err;
    }
    return cap_lua_hw_release(&ud->lease);
}

static int lua_module_dht_close(lua_State *L)
{
    lua_module_dht_ud_t *ud = luaL_checkudata(L, 1, LUA_MODULE_DHT_METATABLE);
    esp_err_t err = lua_module_dht_release(ud);
    if (err != ESP_OK) return luaL_error(L, "dht close failed: %s", esp_err_to_name(err));
    return 0;
}

static int lua_module_dht_gc(lua_State *L)
{
    lua_module_dht_ud_t *ud = luaL_checkudata(L, 1, LUA_MODULE_DHT_METATABLE);
    esp_err_t err = lua_module_dht_release(ud);
    if (err != ESP_OK) ESP_LOGE(TAG, "DHT cleanup failed: %s", esp_err_to_name(err));
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

    vTaskDelay(pdMS_TO_TICKS(LUA_MODULE_DHT_PRE_READ_DELAY_US / 1000) + 1);
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
    int pin = option_int(L, "pin", -1, 0, GPIO_NUM_MAX - 1);
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return luaL_error(L, "dht pin must support input and output");

    lua_getfield(L, 1, "sensor_type");
    const char *sensor_type_str = lua_isnoneornil(L, -1) ? NULL : luaL_checkstring(L, -1);
    dht_sensor_type_t sensor_type = lua_module_dht_sensor_type_from_string(sensor_type_str);
    lua_pop(L, 1);

    if ((int)sensor_type < 0) {
        return luaL_error(L, "environmental_sensor.new: invalid dht sensor_type");
    }

    lua_module_dht_ud_t *ud = (lua_module_dht_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->closed = true;
    ud->pin = pin;
    ud->sensor_type = sensor_type;

    luaL_setmetatable(L, LUA_MODULE_DHT_METATABLE);
    esp_err_t err = cap_lua_hw_claim_gpio(L, &ud->lease, "lua/dht", pin);
    if (err != ESP_OK) return luaL_error(L, "dht pin claim failed: %s", esp_err_to_name(err));
    ud->closed = false;
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

static int lua_module_environmental_sensor_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argcheck(L, lua_gettop(L) == 1, 1, "expected one options table");
    lua_getfield(L, 1, "type");
    const char *type = luaL_checkstring(L, -1);
    bool dht = strcmp(type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_DHT) == 0;
    if (!dht && strcmp(type, LUA_MODULE_ENVIRONMENTAL_SENSOR_TYPE_BME690) != 0) {
        return luaL_error(L, "environmental_sensor: unsupported type '%s'", type);
    }
    lua_pop(L, 1);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        bool valid = key && (strcmp(key, "type") == 0 || (dht ?
            strcmp(key, "pin") == 0 || strcmp(key, "sensor_type") == 0 :
            strcmp(key, "bus") == 0 || strcmp(key, "address") == 0 || strcmp(key, "frequency") == 0 ||
            strcmp(key, "heater_temp") == 0 || strcmp(key, "heater_duration") == 0));
        if (!valid) return luaL_error(L, "environmental_sensor: unsupported option");
        lua_pop(L, 1);
    }
    if (dht) {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_DHT
        return lua_module_dht_new(L);
#endif
    } else {
#if CONFIG_LUA_MODULE_ENVIRONMENTAL_SENSOR_BACKEND_BME690
        return lua_module_bme690_new(L);
#endif
    }
    return luaL_error(L, "environmental_sensor: backend not enabled");
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
