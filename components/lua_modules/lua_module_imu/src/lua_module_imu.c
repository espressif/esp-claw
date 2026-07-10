/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_imu.h"

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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#include "lua_module_imu_backend.h"

#define LUA_IMU_METATABLE        "imu.device"
#define LUA_IMU_DEFAULT_DEVICE   "imu_sensor"
#define LUA_IMU_MAX_NAME_LEN     64
#define LUA_IMU_DEFAULT_FREQ_HZ  400000

typedef struct {
    lua_imu_backend_ctx_t ctx;
    char peripheral_name[LUA_IMU_MAX_NAME_LEN];
    bool peripheral_ref_held;
    gpio_num_t int_gpio_num;
    gpio_num_t sdo_gpio_num;
    bool sensor_initialized;
} lua_imu_handle_t;

typedef struct {
    lua_imu_handle_t *handle;
    char device_name[LUA_IMU_MAX_NAME_LEN];
} lua_imu_ud_t;

/*
 * Local mirror of the dev_custom_imu_sensor_config_t struct that the ESP
 * Board Manager auto-generates from the board's board_devices.yaml.
 *
 * IMPORTANT: This MUST be byte-for-byte identical to the auto-generated
 * struct. The auto-generator emits fields in the order they appear in the
 * YAML schema, and silently omits any field missing from the YAML. If a
 * board's YAML drops a field, the generated struct shrinks and our cast
 * would read garbage from neighbouring fields (e.g. mistaking the next
 * field as `sdo_gpio_num` and "discovering" GPIO0).
 *
 * To make such a divergence impossible, `lua_imu_resolve_board_cfg()`
 * below cross-checks the size reported by the board manager descriptor
 * against `sizeof(lua_imu_board_cfg_t)` and refuses to use the config if
 * they differ. When that fails, fix the board's board_devices.yaml so
 * every field listed here is present (use `-1` for unused GPIOs).
 */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t      i2c_addr;
    int32_t     frequency;
    int8_t      int_gpio_num;
    int8_t      sdo_gpio_num;
    uint8_t     peripheral_count;
    const char *peripheral_name;
} lua_imu_board_cfg_t;

typedef struct {
    char peripheral_name[LUA_IMU_MAX_NAME_LEN];
    int  i2c_addr;
    int  frequency;
    int  int_gpio_num;
    int  sdo_gpio_num;
    bool has_peripheral;
    bool has_i2c_addr;
    bool has_frequency;
    bool has_int_gpio;
    bool has_sdo_gpio;
} lua_imu_resolved_cfg_t;

static const char *TAG = "lua_module_imu";

static void lua_imu_destroy_handle(lua_imu_handle_t *handle);

/* ---------------------------------------------------------------------------
 * Helper exported to backends.
 * ------------------------------------------------------------------------- */

esp_err_t lua_imu_ctx_select_addr(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
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
        ESP_LOGE(TAG, "Failed to create IMU I2C device for address 0x%02x", i2c_addr);
        return ESP_FAIL;
    }
    ctx->i2c_addr = i2c_addr;
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * GPIO + I2C bus setup.
 * ------------------------------------------------------------------------- */

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

static esp_err_t lua_imu_open_i2c_bus(const char *peripheral_name,
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
        ESP_LOGE(TAG, "Failed to get board I2C config '%s': %s",
                 peripheral_name, esp_err_to_name(err));
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

/* ---------------------------------------------------------------------------
 * Handle lifecycle (delegates chip work to the backend).
 * ------------------------------------------------------------------------- */

static esp_err_t lua_imu_create_handle(const lua_imu_resolved_cfg_t *cfg,
                                       lua_imu_handle_t **out_handle)
{
    lua_imu_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (lua_imu_backend.state_size > 0) {
        handle->ctx.state = calloc(1, lua_imu_backend.state_size);
        if (handle->ctx.state == NULL) {
            free(handle);
            return ESP_ERR_NO_MEM;
        }
    }

    snprintf(handle->peripheral_name, sizeof(handle->peripheral_name), "%s", cfg->peripheral_name);
    handle->int_gpio_num = (gpio_num_t)cfg->int_gpio_num;
    handle->sdo_gpio_num = (gpio_num_t)cfg->sdo_gpio_num;

    /* Only touch the SDO pin when the user/board explicitly supplied one.
     * If not provided (sdo_gpio_num < 0), we MUST leave GPIO0 (and every
     * other pin) completely untouched. */
    int sdo_level = 0;
    if (cfg->sdo_gpio_num >= 0 && lua_imu_backend.sdo_level_for_addr != NULL) {
        sdo_level = lua_imu_backend.sdo_level_for_addr((uint8_t)cfg->i2c_addr);
    }
    esp_err_t err = lua_imu_configure_sdo_pin(cfg->sdo_gpio_num, sdo_level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure SDO pin GPIO%d: %s",
                 cfg->sdo_gpio_num, esp_err_to_name(err));
        goto cleanup;
    }

    err = lua_imu_configure_interrupt_pin(cfg->int_gpio_num);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = lua_imu_open_i2c_bus(cfg->peripheral_name, cfg->frequency,
                               &handle->ctx.i2c_bus_handle, &handle->peripheral_ref_held);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = lua_imu_backend.probe(&handle->ctx, (uint8_t)cfg->i2c_addr);
    if (err != ESP_OK) {
        lua_imu_destroy_handle(handle);
        return err;
    }

    handle->sensor_initialized = true;
    *out_handle = handle;
    if (cfg->int_gpio_num >= 0) {
        ESP_LOGI(TAG, "%s IMU initialized on %s, INT GPIO%d, addr 0x%02x, freq %d Hz",
                 lua_imu_backend.chip_name, cfg->peripheral_name,
                 cfg->int_gpio_num, handle->ctx.i2c_addr, cfg->frequency);
    } else {
        ESP_LOGI(TAG, "%s IMU initialized on %s, addr 0x%02x, freq %d Hz",
                 lua_imu_backend.chip_name, cfg->peripheral_name,
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

static void lua_imu_destroy_handle(lua_imu_handle_t *handle)
{
    if (handle == NULL) {
        return;
    }
    if (lua_imu_backend.destroy != NULL) {
        lua_imu_backend.destroy(&handle->ctx);
    }
    if (handle->ctx.i2c_dev_handle != NULL) {
        i2c_bus_device_delete(&handle->ctx.i2c_dev_handle);
        handle->ctx.i2c_dev_handle = NULL;
    }
    if (handle->ctx.i2c_bus_handle != NULL) {
        if (handle->peripheral_ref_held) {
            /* The board manager owns the shared master bus lifecycle. */
            handle->ctx.i2c_bus_handle = NULL;
        } else {
            i2c_bus_delete(&handle->ctx.i2c_bus_handle);
        }
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
    handle->sensor_initialized = false;
    free(handle);
}

/* ---------------------------------------------------------------------------
 * Lua bindings.
 * ------------------------------------------------------------------------- */

static lua_imu_ud_t *lua_imu_get_ud(lua_State *L, int idx)
{
    lua_imu_ud_t *ud = (lua_imu_ud_t *)luaL_checkudata(L, idx, LUA_IMU_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sensor_initialized) {
        luaL_error(L, "imu: invalid or closed handle");
    }
    return ud;
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

static int lua_imu_close_impl(lua_State *L, lua_imu_ud_t *ud)
{
    (void)L;
    if (ud->handle != NULL) {
        lua_imu_destroy_handle(ud->handle);
        ud->handle = NULL;
    }
    ud->device_name[0] = '\0';
    return 0;
}

static int lua_imu_gc(lua_State *L)
{
    lua_imu_ud_t *ud = (lua_imu_ud_t *)luaL_testudata(L, 1, LUA_IMU_METATABLE);
    if (ud && ud->handle) {
        return lua_imu_close_impl(L, ud);
    }
    return 0;
}

static int lua_imu_close(lua_State *L)
{
    lua_imu_ud_t *ud = (lua_imu_ud_t *)luaL_checkudata(L, 1, LUA_IMU_METATABLE);
    if (ud->handle) {
        return lua_imu_close_impl(L, ud);
    }
    return 0;
}

static int lua_imu_name(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static int lua_imu_read(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    lua_imu_sample_t sample = { 0 };
    if (lua_imu_backend.read_sample(&ud->handle->ctx, &sample) != ESP_OK) {
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
    if (lua_imu_backend.read_temperature(&ud->handle->ctx, &temp) != ESP_OK) {
        return luaL_error(L, "imu read_temperature failed");
    }
    lua_pushinteger(L, temp);
    return 1;
}

static int lua_imu_read_int_status(lua_State *L)
{
    lua_imu_ud_t *ud = lua_imu_get_ud(L, 1);
    uint32_t status = 0;
    if (lua_imu_backend.read_int_status(&ud->handle->ctx, &status) != ESP_OK) {
        return luaL_error(L, "imu read_int_status failed");
    }
    lua_pushinteger(L, (lua_Integer)status);
    return 1;
}

/* ---------------------------------------------------------------------------
 * Board defaults + Lua options resolution.
 * ------------------------------------------------------------------------- */

/*
 * Resolve the auto-generated board device config for `device_name` and
 * verify that the layout we expect matches the size emitted by the board
 * manager. The descriptor list is exported by the auto-generator as the
 * (de-facto stable) symbol `g_esp_board_devices[]`; we walk it ourselves
 * to get at `cfg_size` since the public getter only returns a void*.
 *
 * If the size mismatches, the board's YAML is out of sync with the field
 * list in `lua_imu_board_cfg_t`. Refuse to use the config rather than
 * silently misinterpret bytes (this is exactly what would otherwise turn
 * a missing `sdo_gpio_num` into a phantom "GPIO0 is the SDO strap").
 */
static esp_err_t lua_imu_resolve_board_cfg(const char *device_name,
                                           const lua_imu_board_cfg_t **out)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    const esp_board_device_desc_t *desc = g_esp_board_devices;
    while (desc != NULL && desc->name != NULL) {
        if (strcmp(desc->name, device_name) == 0) {
            if (desc->cfg == NULL) {
                return ESP_ERR_NOT_FOUND;
            }
            if (desc->cfg_size != sizeof(lua_imu_board_cfg_t)) {
                ESP_LOGE(TAG,
                         "Board device '%s' cfg_size=%u differs from expected %u; "
                         "board_devices.yaml schema is out of sync with lua_imu_board_cfg_t. "
                         "Every field listed in lua_imu_board_cfg_t MUST be present in YAML "
                         "(use -1 for unused GPIOs).",
                         device_name,
                         (unsigned)desc->cfg_size,
                         (unsigned)sizeof(lua_imu_board_cfg_t));
                return ESP_ERR_INVALID_SIZE;
            }
            *out = (const lua_imu_board_cfg_t *)desc->cfg;
            return ESP_OK;
        }
        desc = desc->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t lua_imu_load_board_defaults(const char *device_name,
                                             lua_imu_resolved_cfg_t *out)
{
    const lua_imu_board_cfg_t *board = NULL;
    esp_err_t err = lua_imu_resolve_board_cfg(device_name, &board);
    if (err != ESP_OK) {
        return err;
    }

    if (board->chip != NULL && strcmp(board->chip, lua_imu_backend.chip_name) != 0) {
        ESP_LOGW(TAG, "Board device '%s' chip='%s' does not match %s backend",
                 device_name, board->chip, lua_imu_backend.chip_name);
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
    /* The size check above guarantees `board` has every field listed in our
     * mirror struct. Only adopt the GPIO when explicitly non-negative; YAML
     * uses `-1` to mean "no strap wired", and any value >= 0 is taken as a
     * real GPIO number. */
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

static void lua_imu_apply_lua_overrides(lua_State *L, int opts_idx,
                                        lua_imu_resolved_cfg_t *cfg)
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

    /* sdo_gpio: opt-in only. Passing a negative number, or omitting the
     * key entirely, leaves sdo_gpio_num at -1 and the main module will
     * not touch any GPIO. Do NOT default to 0 here. */
    lua_getfield(L, opts_idx, "sdo_gpio");
    if (lua_isnumber(L, -1)) {
        cfg->sdo_gpio_num = (int)lua_tointeger(L, -1);
        cfg->has_sdo_gpio = true;
    }
    lua_pop(L, 1);
}

static int lua_imu_new(lua_State *L)
{
    const char *device_name = LUA_IMU_DEFAULT_DEVICE;
    int opts_idx = 0;

    /*
     * Accepted call shapes:
     *   imu.new()
     *   imu.new("imu_sensor")
     *   imu.new({ ... })
     *   imu.new("imu_sensor", { ... })
     */
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

    if (strlen(device_name) >= LUA_IMU_MAX_NAME_LEN) {
        return luaL_error(L, "imu device name too long");
    }

    lua_imu_resolved_cfg_t cfg = { 0 };
    cfg.int_gpio_num = -1;
    cfg.sdo_gpio_num = -1;

    /* Try the requested device, then fall back to legacy "<chip>_sensor". */
    char legacy_name[LUA_IMU_MAX_NAME_LEN];
    snprintf(legacy_name, sizeof(legacy_name), "%s_sensor", lua_imu_backend.chip_name);

    esp_err_t err = lua_imu_load_board_defaults(device_name, &cfg);
    const char *opened_device_name = device_name;
    /* A size mismatch means YAML and lua_imu_board_cfg_t disagree -- this is
     * a board-side bug and we must surface it instead of silently retrying
     * a different name. ESP_ERR_NOT_FOUND, on the other hand, just means
     * the board did not declare this device, so the legacy fallback is OK. */
    if (err == ESP_ERR_INVALID_SIZE) {
        return luaL_error(L,
                          "imu.new: board device '%s' config schema mismatch "
                          "(see error log above for details)", device_name);
    }
    if (err != ESP_OK && strcmp(device_name, LUA_IMU_DEFAULT_DEVICE) == 0) {
        esp_err_t legacy_err = lua_imu_load_board_defaults(legacy_name, &cfg);
        if (legacy_err == ESP_OK) {
            opened_device_name = legacy_name;
            ESP_LOGW(TAG, "Default device '%s' not declared, using legacy '%s'",
                     LUA_IMU_DEFAULT_DEVICE, legacy_name);
        } else if (legacy_err == ESP_ERR_INVALID_SIZE) {
            return luaL_error(L,
                              "imu.new: legacy board device '%s' config schema mismatch",
                              legacy_name);
        }
    }

    lua_imu_apply_lua_overrides(L, opts_idx, &cfg);

    if (!cfg.has_peripheral) {
        return luaL_error(L, "imu.new: missing 'peripheral' (board declares no '%s', "
                              "and no override given)", device_name);
    }
    if (!cfg.has_i2c_addr) {
        cfg.i2c_addr = lua_imu_backend.default_addr();
        cfg.has_i2c_addr = true;
    }
    if (!cfg.has_frequency) {
        cfg.frequency = LUA_IMU_DEFAULT_FREQ_HZ;
        cfg.has_frequency = true;
    }
    if (!lua_imu_backend.is_supported_addr((uint8_t)cfg.i2c_addr)) {
        return luaL_error(L, "imu.new: unsupported %s I2C address 0x%02x",
                          lua_imu_backend.chip_name, cfg.i2c_addr);
    }

    lua_imu_handle_t *handle = NULL;
    err = lua_imu_create_handle(&cfg, &handle);
    if (err != ESP_OK || handle == NULL) {
        return luaL_error(L, "imu.new failed: %s",
                          esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
    }

    lua_imu_ud_t *ud = (lua_imu_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->handle = handle;
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", opened_device_name);

    luaL_getmetatable(L, LUA_IMU_METATABLE);
    lua_setmetatable(L, -2);
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
