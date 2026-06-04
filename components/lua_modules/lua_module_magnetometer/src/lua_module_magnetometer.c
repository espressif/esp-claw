/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_magnetometer.h"

#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/gpio.h"
#include "esp_board_device.h"
#include "esp_board_manager.h"
#include "esp_board_periph.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lauxlib.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "lua_module_mag_backend.h"

#define LUA_MAG_METATABLE        "magnetometer.device"
#define LUA_MAG_DEFAULT_DEVICE   "magnetometer_sensor"
#define LUA_MAG_MAX_NAME_LEN     64
#define LUA_MAG_NVS_KEY_HARD     "hard_iron"
#define LUA_MAG_NVS_KEY_SOFT     "soft_iron"
#define LUA_MAG_NVS_KEY_CAL      "calibrated"

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
    char peripheral_name[LUA_MAG_MAX_NAME_LEN];
    bool peripheral_ref_held;
    gpio_num_t int_gpio_num;
    gpio_num_t sdo_gpio_num;
    bool sensor_initialized;
    lua_mag_calibration_t calibration;
} lua_mag_handle_t;

typedef struct {
    lua_mag_handle_t *handle;
    char device_name[LUA_MAG_MAX_NAME_LEN];
} lua_mag_ud_t;

/*
 * Local mirror of the dev_custom_magnetometer_sensor_config_t struct that
 * the ESP Board Manager auto-generates from the board's board_devices.yaml.
 *
 * IMPORTANT: This MUST be byte-for-byte identical to the auto-generated
 * struct. See `lua_mag_resolve_board_cfg()` below: the size of this struct
 * is cross-checked against the board manager descriptor's `cfg_size`, and
 * the magnetometer refuses to start if they differ. That prevents a board
 * whose YAML drops e.g. `sdo_gpio_num` from silently re-aliasing the next
 * field as the SDO GPIO (which would otherwise hijack GPIO0).
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t i2c_addr;
    int32_t frequency;
    int8_t int_gpio_num;
    int8_t sdo_gpio_num;
    uint8_t peripheral_count;
    const char *peripheral_name;
} lua_mag_board_cfg_t;

typedef struct {
    char peripheral_name[LUA_MAG_MAX_NAME_LEN];
    int i2c_addr;
    int frequency;
    int int_gpio_num;
    int sdo_gpio_num;
    bool has_peripheral;
    bool has_i2c_addr;
    bool has_frequency;
    bool has_int_gpio;
    bool has_sdo_gpio;
    bool try_alt_i2c_addr;
} lua_mag_resolved_cfg_t;

static const char *TAG = "lua_module_magnetometer";

static void lua_mag_destroy_handle(lua_mag_handle_t *handle);

/* ---------------------------------------------------------------------------
 * Helpers exported to chip backends (declared in lua_module_mag_backend.h).
 * ------------------------------------------------------------------------- */

void lua_mag_delay_us(uint32_t period_us)
{
    if (period_us < 1000) {
        esp_rom_delay_us(period_us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    }
}

esp_err_t lua_mag_ctx_select_addr(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    if (ctx->i2c_dev_handle != NULL && ctx->i2c_addr == i2c_addr) {
        return ESP_OK;
    }
    if (ctx->i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&ctx->i2c_dev_handle);
        ctx->i2c_dev_handle = NULL;
    }
    ctx->i2c_dev_handle = i2c_bus_device_create(ctx->i2c_bus_handle, i2c_addr, 0);
    if (ctx->i2c_dev_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create magnetometer I2C device for address 0x%02x", i2c_addr);
        return ESP_FAIL;
    }
    ctx->i2c_addr = i2c_addr;
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Calibration: NVS persistence + hard/soft-iron math (chip-agnostic).
 * ------------------------------------------------------------------------- */

static void lua_mag_calibration_reset_state(lua_mag_handle_t *handle)
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

static uint32_t lua_mag_hash_name(const char *name)
{
    uint32_t hash = 2166136261u;
    while (name != NULL && *name != '\0') {
        hash ^= (uint8_t)*name++;
        hash *= 16777619u;
    }
    return hash;
}

static esp_err_t lua_mag_open_nvs(const char *device_name, nvs_handle_t *nvs_handle)
{
    char namespace_name[16];
    esp_err_t err;

    snprintf(namespace_name, sizeof(namespace_name), "mag%08" PRIx32,
             lua_mag_hash_name(device_name));

    err = nvs_open(namespace_name, NVS_READWRITE, nvs_handle);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS partition");
            err = nvs_flash_init();
        }
        if (err != ESP_OK) {
            return err;
        }
        return nvs_open(namespace_name, NVS_READWRITE, nvs_handle);
    }
    return err;
}

static esp_err_t lua_mag_save_calibration(const char *device_name,
                                          const lua_mag_calibration_t *cal)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = lua_mag_open_nvs(device_name, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs_handle, LUA_MAG_NVS_KEY_HARD, cal->hard_iron, sizeof(cal->hard_iron));
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs_handle, LUA_MAG_NVS_KEY_SOFT, cal->soft_iron, sizeof(cal->soft_iron));
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs_handle, LUA_MAG_NVS_KEY_CAL, cal->calibrated ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t lua_mag_load_calibration(const char *device_name, lua_mag_calibration_t *cal)
{
    nvs_handle_t nvs_handle;
    size_t hard_size = sizeof(cal->hard_iron);
    size_t soft_size = sizeof(cal->soft_iron);
    uint8_t calibrated = 0;
    esp_err_t err = lua_mag_open_nvs(device_name, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_blob(nvs_handle, LUA_MAG_NVS_KEY_HARD, cal->hard_iron, &hard_size);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs_handle, LUA_MAG_NVS_KEY_SOFT, cal->soft_iron, &soft_size);
    }
    if (err == ESP_OK) {
        err = nvs_get_u8(nvs_handle, LUA_MAG_NVS_KEY_CAL, &calibrated);
    }
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    cal->calibrated = (calibrated != 0);
    return ESP_OK;
}

static esp_err_t lua_mag_clear_calibration_storage(const char *device_name)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = lua_mag_open_nvs(device_name, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    const char *keys[] = { LUA_MAG_NVS_KEY_HARD, LUA_MAG_NVS_KEY_SOFT, LUA_MAG_NVS_KEY_CAL };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && err == ESP_OK; i++) {
        err = nvs_erase_key(nvs_handle, keys[i]);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
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

static void lua_mag_calibration_record_sample(lua_mag_handle_t *handle, const float sample[3])
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

static esp_err_t lua_mag_calibration_finish(lua_mag_handle_t *handle)
{
    float avg_delta[3];

    if (handle->calibration.sample_count < 16) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < 3; i++) {
        handle->calibration.hard_iron[i] =
            (handle->calibration.mag_max[i] + handle->calibration.mag_min[i]) * 0.5f;
        avg_delta[i] =
            (handle->calibration.mag_max[i] - handle->calibration.mag_min[i]) * 0.5f;
        if (avg_delta[i] <= 0.0f) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    float avg_radius = (avg_delta[0] + avg_delta[1] + avg_delta[2]) / 3.0f;
    if (avg_radius <= 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < 3; i++) {
        for (size_t j = 0; j < 3; j++) {
            handle->calibration.soft_iron[i][j] = (i == j) ? (avg_radius / avg_delta[i]) : 0.0f;
        }
    }
    handle->calibration.calibrated = true;
    handle->calibration.collecting = false;
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Lua <-> C value plumbing.
 * ------------------------------------------------------------------------- */

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
        if (!lua_isnumber(L, -1)) {
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
                if (!lua_isnumber(L, -1)) {
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

/* ---------------------------------------------------------------------------
 * GPIO + I2C bus setup (board-level, not chip-specific).
 * ------------------------------------------------------------------------- */

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

static esp_err_t lua_mag_configure_sdo_pin(int sdo_gpio_num)
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
    return gpio_set_level((gpio_num_t)sdo_gpio_num, 0);
}

static esp_err_t lua_mag_open_i2c_bus(const char *peripheral_name, int frequency,
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
        ESP_LOGE(TAG, "Failed to get board I2C config '%s': %s",
                 peripheral_name, esp_err_to_name(err));
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return err;
    }

    if (!i2c_master_cfg->flags.enable_internal_pullup) {
        ESP_LOGW(TAG,
                 "Board I2C '%s' has internal pull-ups disabled; enabling pull-ups for magnetometer",
                 peripheral_name);
        ESP_RETURN_ON_ERROR(gpio_pullup_en(i2c_master_cfg->sda_io_num), TAG,
                            "Failed to enable SDA pull-up on GPIO%d", i2c_master_cfg->sda_io_num);
        ESP_RETURN_ON_ERROR(gpio_pullup_en(i2c_master_cfg->scl_io_num), TAG,
                            "Failed to enable SCL pull-up on GPIO%d", i2c_master_cfg->scl_io_num);
    }

    const i2c_config_t bus_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = i2c_master_cfg->sda_io_num,
        .scl_io_num = i2c_master_cfg->scl_io_num,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = (uint32_t)frequency,
        .clk_flags = 0,
    };

    (void)i2c_master_handle;
    *i2c_bus_handle = i2c_bus_create(i2c_master_cfg->i2c_port, &bus_cfg);
    if (*i2c_bus_handle == NULL) {
        esp_board_periph_unref_handle(peripheral_name);
        *peripheral_ref_held = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * Handle lifecycle (delegates chip work to the backend).
 * ------------------------------------------------------------------------- */

static esp_err_t lua_mag_create_handle(const lua_mag_resolved_cfg_t *cfg,
                                       lua_mag_handle_t **out_handle)
{
    lua_mag_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (lua_mag_backend.state_size > 0) {
        handle->ctx.state = calloc(1, lua_mag_backend.state_size);
        if (handle->ctx.state == NULL) {
            free(handle);
            return ESP_ERR_NO_MEM;
        }
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->int_gpio_num = (gpio_num_t)cfg->int_gpio_num;
    handle->sdo_gpio_num = (gpio_num_t)cfg->sdo_gpio_num;

    esp_err_t err = lua_mag_configure_sdo_pin(cfg->sdo_gpio_num);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SDO pin GPIO%d: %s",
                 cfg->sdo_gpio_num, esp_err_to_name(err));
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    err = lua_mag_configure_interrupt_pin(cfg->int_gpio_num);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = lua_mag_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                               &handle->ctx.i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = lua_mag_backend.probe(&handle->ctx, (uint8_t)cfg->i2c_addr);
    if (err != ESP_OK && cfg->try_alt_i2c_addr && lua_mag_backend.probe_alternates != NULL) {
        err = lua_mag_backend.probe_alternates(&handle->ctx, (uint8_t)cfg->i2c_addr);
    }
    if (err != ESP_OK) {
        lua_mag_destroy_handle(handle);
        return err;
    }

    handle->sensor_initialized = true;
    *out_handle = handle;
    if (cfg->int_gpio_num >= 0) {
        ESP_LOGI(TAG, "%s initialized on %s, INT GPIO%d, addr 0x%02x, freq %d Hz",
                 lua_mag_backend.chip_name, cfg->peripheral_name, cfg->int_gpio_num,
                 handle->ctx.i2c_addr, cfg->frequency);
    } else {
        ESP_LOGI(TAG, "%s initialized on %s, addr 0x%02x, freq %d Hz",
                 lua_mag_backend.chip_name, cfg->peripheral_name,
                 handle->ctx.i2c_addr, cfg->frequency);
    }
    return ESP_OK;

cleanup:
    if (handle->ctx.state != NULL) {
        free(handle->ctx.state);
    }
    free(handle);
    return err;
}

static void lua_mag_destroy_handle(lua_mag_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->ctx.i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->ctx.i2c_dev_handle);
        handle->ctx.i2c_dev_handle = NULL;
    }
    if (handle->int_gpio_num >= 0) {
        gpio_reset_pin(handle->int_gpio_num);
    }
    if (handle->sdo_gpio_num >= 0) {
        gpio_reset_pin(handle->sdo_gpio_num);
    }
    if (handle->peripheral_ref_held && handle->peripheral_name[0] != '\0') {
        esp_board_periph_unref_handle(handle->peripheral_name);
    }
    if (handle->ctx.state != NULL) {
        free(handle->ctx.state);
        handle->ctx.state = NULL;
    }
    free(handle);
}

/* ---------------------------------------------------------------------------
 * Lua bindings.
 * ------------------------------------------------------------------------- */

static lua_mag_ud_t *lua_mag_get_ud(lua_State *L, int idx)
{
    lua_mag_ud_t *ud = (lua_mag_ud_t *)luaL_checkudata(L, idx, LUA_MAG_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized) {
        luaL_error(L, "magnetometer: invalid or closed handle");
    }
    return ud;
}

static int lua_mag_close_impl(lua_State *L, lua_mag_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_mag_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_mag_gc(lua_State *L)
{
    lua_mag_ud_t *ud = (lua_mag_ud_t *)luaL_testudata(L, 1, LUA_MAG_METATABLE);
    if (ud && ud->handle) {
        return lua_mag_close_impl(L, ud);
    }
    return 0;
}

static int lua_mag_close(lua_State *L)
{
    lua_mag_ud_t *ud = (lua_mag_ud_t *)luaL_checkudata(L, 1, LUA_MAG_METATABLE);
    if (ud->handle) {
        return lua_mag_close_impl(L, ud);
    }
    return 0;
}

static int lua_mag_name(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_mag_chip_id(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_pushinteger(L, ud->handle->ctx.chip_id);
    return 1;
}

static int lua_mag_read(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_sample_t sample = { 0 };
    float raw[3];
    float corrected[3];

    if (lua_mag_backend.read_sample(&ud->handle->ctx, &sample) != ESP_OK) {
        return luaL_error(L, "magnetometer read failed");
    }

    raw[0] = sample.x;
    raw[1] = sample.y;
    raw[2] = sample.z;
    lua_mag_apply_calibration(&ud->handle->calibration, raw, corrected);

    lua_newtable(L);
    lua_mag_push_axes_table(L, corrected[0], corrected[1], corrected[2]);
    lua_setfield(L, -2, "magnetic");
    lua_mag_push_axes_table(L, raw[0], raw[1], raw[2]);
    lua_setfield(L, -2, "raw_magnetic");
    lua_pushnumber(L, sample.temperature);
    lua_setfield(L, -2, "temperature");
    lua_pushinteger(L, sample.status);
    lua_setfield(L, -2, "status");
    lua_pushboolean(L, ud->handle->calibration.calibrated);
    lua_setfield(L, -2, "calibrated");
    return 1;
}

static int lua_mag_read_temperature(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_sample_t sample = { 0 };
    if (lua_mag_backend.read_sample(&ud->handle->ctx, &sample) != ESP_OK) {
        return luaL_error(L, "magnetometer read_temperature failed");
    }
    lua_pushnumber(L, sample.temperature);
    return 1;
}

static int lua_mag_read_int_status(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    uint8_t status = 0;
    if (lua_mag_backend.read_status(&ud->handle->ctx, &status) != ESP_OK) {
        return luaL_error(L, "magnetometer read_int_status failed");
    }
    lua_pushinteger(L, status);
    return 1;
}

static int lua_mag_calibration_reset(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_calibration_reset_state(ud->handle);
    ud->handle->calibration.collecting = true;
    return 0;
}

static int lua_mag_calibration_add_sample(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    float sample[3];

    if (lua_istable(L, 2)) {
        if (!lua_mag_read_vec3(L, 2, sample)) {
            return luaL_error(L, "magnetometer calibration_add_sample expects {x,y,z} array");
        }
    } else {
        lua_mag_sample_t mag_sample = { 0 };
        if (lua_mag_backend.read_sample(&ud->handle->ctx, &mag_sample) != ESP_OK) {
            return luaL_error(L, "magnetometer calibration_add_sample read failed");
        }
        sample[0] = mag_sample.x;
        sample[1] = mag_sample.y;
        sample[2] = mag_sample.z;
    }

    lua_mag_calibration_record_sample(ud->handle, sample);
    lua_pushinteger(L, (lua_Integer)ud->handle->calibration.sample_count);
    return 1;
}

static int lua_mag_calibration_finish_lua(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    esp_err_t err = lua_mag_calibration_finish(ud->handle);
    if (err != ESP_OK) {
        return luaL_error(L, "magnetometer calibration_finish failed: %s", esp_err_to_name(err));
    }
    err = lua_mag_save_calibration(ud->device_name, &ud->handle->calibration);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
    }
    lua_mag_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static int lua_mag_calibration_get(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static int lua_mag_calibration_set(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    float hard_iron[3];
    float soft_iron[3][3];
    esp_err_t err;

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

    memcpy(ud->handle->calibration.hard_iron, hard_iron, sizeof(hard_iron));
    memcpy(ud->handle->calibration.soft_iron, soft_iron, sizeof(soft_iron));
    ud->handle->calibration.calibrated = true;
    ud->handle->calibration.collecting = false;

    err = lua_mag_save_calibration(ud->device_name, &ud->handle->calibration);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
    }
    lua_mag_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

static int lua_mag_calibration_clear(lua_State *L)
{
    lua_mag_ud_t *ud = lua_mag_get_ud(L, 1);
    lua_mag_calibration_reset_state(ud->handle);
    esp_err_t err = lua_mag_clear_calibration_storage(ud->device_name);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to clear persisted calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
    }
    lua_mag_push_calibration_table(L, &ud->handle->calibration);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Board defaults + Lua options resolution.
 * ------------------------------------------------------------------------- */

/*
 * Walk the board manager descriptor list and verify that the auto-generated
 * config for `device_name` has exactly the layout `lua_mag_board_cfg_t`
 * expects. Returns ESP_ERR_NOT_FOUND if the board doesn't declare the
 * device, and ESP_ERR_INVALID_SIZE if the schema diverges (which is a
 * board-side bug and must be surfaced rather than papered over).
 */
static esp_err_t lua_mag_resolve_board_cfg(const char *device_name,
                                           const lua_mag_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *desc = g_esp_board_devices;
    while (desc != NULL && desc->name != NULL) {
        if (strcmp(desc->name, device_name) == 0) {
            if (desc->cfg == NULL) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(lua_mag_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u differs from expected %u; "
                         "board_devices.yaml schema is out of sync with lua_mag_board_cfg_t. "
                         "Every field listed in lua_mag_board_cfg_t MUST be present in YAML "
                         "(use -1 for unused GPIOs).",
                         device_name,
                         (unsigned)desc->cfg_size,
                         (unsigned)sizeof(lua_mag_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const lua_mag_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t lua_mag_load_board_defaults(const char *device_name,
                                             lua_mag_resolved_cfg_t *out)
{
    const lua_mag_board_cfg_t *board = NULL;
    esp_err_t err = lua_mag_resolve_board_cfg(device_name, &board);
    if (err != ESP_OK) {
        return err;
    }

    if (board->chip != NULL && strcmp(board->chip, lua_mag_backend.chip_name) != 0) {
        ESP_LOGW(TAG, "Board device '%s' chip='%s' does not match %s backend",
                 device_name, board->chip, lua_mag_backend.chip_name);
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
    /* Only adopt board GPIOs when explicitly non-negative. The board manager
     * auto-generator defaults missing int8 fields to 0, which we MUST NOT
     * treat as a real GPIO0 -- doing so would silently reconfigure GPIO0 as
     * an INT input or SDO strap output. Boards that lack the strap should
     * either omit the device entry or set the field to -1 in YAML. */
    if (board->int_gpio_num >= 0) {
        out->int_gpio_num = board->int_gpio_num;
        out->has_int_gpio = true;
    }
    if (board->sdo_gpio_num >= 0) {
        out->sdo_gpio_num = board->sdo_gpio_num;
        out->has_sdo_gpio = true;
    }
    return ESP_OK;
}

static void lua_mag_apply_lua_overrides(lua_State *L, int opts_idx, lua_mag_resolved_cfg_t *cfg)
{
    if (opts_idx == 0 || lua_type(L, opts_idx) != LUA_TTABLE) {
        return;
    }

    lua_getfield(L, opts_idx, "peripheral");
    if (lua_isstring(L, -1)) {
        snprintf(cfg->peripheral_name, sizeof(cfg->peripheral_name), "%s", lua_tostring(L, -1));
        cfg->has_peripheral = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "i2c_addr");
    if (lua_isnumber(L, -1)) {
        cfg->i2c_addr = (int)lua_tointeger(L, -1);
        cfg->has_i2c_addr = true;
        cfg->try_alt_i2c_addr = false;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "frequency");
    if (lua_isnumber(L, -1)) {
        cfg->frequency = (int)lua_tointeger(L, -1);
        cfg->has_frequency = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "int_gpio");
    if (lua_isnumber(L, -1)) {
        cfg->int_gpio_num = (int)lua_tointeger(L, -1);
        cfg->has_int_gpio = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "sdo_gpio");
    if (lua_isnumber(L, -1)) {
        cfg->sdo_gpio_num = (int)lua_tointeger(L, -1);
        cfg->has_sdo_gpio = true;
    }
    lua_pop(L, 1);
}

static int lua_mag_new(lua_State *L)
{
    const char *device_name = LUA_MAG_DEFAULT_DEVICE;
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

    if (strlen(device_name) >= LUA_MAG_MAX_NAME_LEN) {
        return luaL_error(L, "magnetometer device name too long");
    }

    lua_mag_resolved_cfg_t cfg = { 0 };
    cfg.int_gpio_num = -1;
    cfg.sdo_gpio_num = -1;

    /* Try the requested device, then fall back to legacy "<chip>_sensor" name. */
    char legacy_name[LUA_MAG_MAX_NAME_LEN];
    snprintf(legacy_name, sizeof(legacy_name), "%s_sensor", lua_mag_backend.chip_name);

    esp_err_t err = lua_mag_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;
    if (err == ESP_ERR_INVALID_SIZE) {
        return luaL_error(L,
                          "magnetometer.new: board device '%s' config schema mismatch "
                          "(see error log above for details)", device_name);
    }
    if (err != ESP_OK && strcmp(device_name, LUA_MAG_DEFAULT_DEVICE) == 0) {
        esp_err_t legacy_err = lua_mag_load_board_defaults(legacy_name, &cfg);
        if (legacy_err == ESP_OK) {
            opened_device_name = legacy_name;
            ESP_LOGW(TAG, "Default device '%s' not declared, using legacy '%s'",
                     LUA_MAG_DEFAULT_DEVICE, legacy_name);
        } else if (legacy_err == ESP_ERR_INVALID_SIZE) {
            return luaL_error(L,
                              "magnetometer.new: legacy board device '%s' config schema mismatch",
                              legacy_name);
        }
    }

    lua_mag_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "magnetometer.new: missing 'peripheral' (board declares no '%s', "
                              "and no override given)", device_name);
    }
    if (!cfg.has_i2c_addr) {
        cfg.i2c_addr = lua_mag_backend.default_addr();
        cfg.has_i2c_addr = true;
        cfg.try_alt_i2c_addr = true;
    }
    if (!cfg.has_frequency) {
        cfg.frequency = 100000;
        cfg.has_frequency = true;
    }
    if (!cfg.has_int_gpio) {
        cfg.int_gpio_num = -1;
    }
    if (!cfg.has_sdo_gpio) {
        cfg.sdo_gpio_num = -1;
    }

    if (!lua_mag_backend.is_supported_addr((uint8_t)cfg.i2c_addr)) {
        return luaL_error(L, "magnetometer.new: unsupported %s I2C address 0x%02x",
                          lua_mag_backend.chip_name, cfg.i2c_addr);
    }

    lua_mag_handle_t *handle = NULL;
    err = lua_mag_create_handle(&cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "magnetometer.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_mag_ud_t *ud = (lua_mag_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);
    lua_mag_calibration_reset_state(handle);
    err = lua_mag_load_calibration(ud->device_name, &handle->calibration);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load calibration for %s: %s",
                 ud->device_name, esp_err_to_name(err));
        lua_mag_calibration_reset_state(handle);
    }

    luaL_getmetatable(L, LUA_MAG_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_magnetometer(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MAG_METATABLE)) {
        lua_pushcfunction(L, lua_mag_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_mag_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_mag_read_temperature);
        lua_setfield(L, -2, "read_temperature");
        lua_pushcfunction(L, lua_mag_read_int_status);
        lua_setfield(L, -2, "read_int_status");
        lua_pushcfunction(L, lua_mag_chip_id);
        lua_setfield(L, -2, "chip_id");
        lua_pushcfunction(L, lua_mag_calibration_reset);
        lua_setfield(L, -2, "calibration_reset");
        lua_pushcfunction(L, lua_mag_calibration_add_sample);
        lua_setfield(L, -2, "calibration_add_sample");
        lua_pushcfunction(L, lua_mag_calibration_finish_lua);
        lua_setfield(L, -2, "calibration_finish");
        lua_pushcfunction(L, lua_mag_calibration_get);
        lua_setfield(L, -2, "calibration_get");
        lua_pushcfunction(L, lua_mag_calibration_set);
        lua_setfield(L, -2, "calibration_set");
        lua_pushcfunction(L, lua_mag_calibration_clear);
        lua_setfield(L, -2, "calibration_clear");
        lua_pushcfunction(L, lua_mag_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_mag_close);
        lua_setfield(L, -2, "close");
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
