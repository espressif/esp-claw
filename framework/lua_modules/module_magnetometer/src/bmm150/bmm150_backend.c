/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BMM150 backend for lua_module_magnetometer (vendored Bosch BMM150 SensorAPI).
 */

#include "sdkconfig.h"

#if CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"

#include "bmm150.h"
#include "bmm150_defs.h"
#include "lua_module_mag_backend.h"

static const char *TAG = "lua_mag_bmm150";

typedef struct {
    struct bmm150_dev dev;
    lua_mag_backend_ctx_t *ctx;
} bmm150_state_t;

static BMM150_INTF_RET_TYPE bmm150_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                            uint32_t length, void *intf_ptr)
{
    bmm150_state_t *st = (bmm150_state_t *)intf_ptr;
    if (st == NULL || st->ctx == NULL || st->ctx->i2c_dev_handle == NULL) {
        return BMM150_E_COM_FAIL;
    }
    esp_err_t err = i2c_bus_read_bytes(st->ctx->i2c_dev_handle, reg_addr,
                                       (uint16_t)length, reg_data);
    return (err == ESP_OK) ? BMM150_INTF_RET_SUCCESS : BMM150_E_COM_FAIL;
}

static BMM150_INTF_RET_TYPE bmm150_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                             uint32_t length, void *intf_ptr)
{
    bmm150_state_t *st = (bmm150_state_t *)intf_ptr;
    if (st == NULL || st->ctx == NULL || st->ctx->i2c_dev_handle == NULL) {
        return BMM150_E_COM_FAIL;
    }
    esp_err_t err = i2c_bus_write_bytes(st->ctx->i2c_dev_handle, reg_addr,
                                        (uint16_t)length, reg_data);
    return (err == ESP_OK) ? BMM150_INTF_RET_SUCCESS : BMM150_E_COM_FAIL;
}

static void bmm150_delay_us_cb(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    lua_mag_delay_us(period_us);
}

/*
 * BMM150 powers up in suspend mode where the entire register map is
 * inaccessible (I2C reads return 0x00). Must write POWER_CONTROL=0x01 first
 * to bring it from suspend to sleep before bmm150_init can read the proper
 * chip_id (0x32). The typical workflow then runs in FORCED mode where each
 * read_mag_data triggers a single conversion. See the reference impl in
 * esp-vocat-base/.../magnetic_slide_switch/profiles/base.
 */
static esp_err_t bmm150_power_on(bmm150_state_t *st)
{
    uint8_t power = 0x01;
    int8_t rslt = bmm150_set_regs(BMM150_REG_POWER_CONTROL, &power, 1, &st->dev);
    if (rslt != BMM150_OK) {
        ESP_LOGW(TAG, "BMM150 power-on write failed: %d", rslt);
        return ESP_FAIL;
    }
    /* BMM150_START_UP_TIME is already in microseconds (3000us = 3ms). */
    lua_mag_delay_us(BMM150_START_UP_TIME);
    return ESP_OK;
}

static esp_err_t apply_default_runtime_config(bmm150_state_t *st)
{
    struct bmm150_settings settings = { 0 };
    int8_t rslt;

    /* Minimum repetition -> fastest conversion. In FORCED mode each
     * read_mag_data call triggers a single sampling cycle. */
    uint8_t rep_xy = 0x00;
    uint8_t rep_z = 0x00;
    rslt = bmm150_set_regs(BMM150_REG_REP_XY, &rep_xy, 1, &st->dev);
    rslt |= bmm150_set_regs(BMM150_REG_REP_Z, &rep_z, 1, &st->dev);
    if (rslt != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150 set repetition failed: %d", rslt);
        return ESP_FAIL;
    }

    settings.pwr_mode = BMM150_POWERMODE_FORCED;
    rslt = bmm150_set_op_mode(&settings, &st->dev);
    if (rslt != BMM150_OK) {
        ESP_LOGE(TAG, "BMM150 set FORCED mode failed: %d", rslt);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BMM150 configured in FORCED mode, chip_id=0x%02X", st->dev.chip_id);
    return ESP_OK;
}

static esp_err_t bmm150_probe(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    bmm150_state_t *st = (bmm150_state_t *)ctx->state;
    int8_t rslt;

    ESP_RETURN_ON_ERROR(lua_mag_ctx_select_addr(ctx, i2c_addr), TAG,
                        "Failed to select BMM150 I2C address 0x%02x", i2c_addr);

    memset(&st->dev, 0, sizeof(st->dev));
    st->ctx = ctx;
    st->dev.intf = BMM150_I2C_INTF;
    st->dev.read = bmm150_i2c_read;
    st->dev.write = bmm150_i2c_write;
    st->dev.delay_us = bmm150_delay_us_cb;
    st->dev.intf_ptr = st;

    /* Step 1: bring the chip from suspend to sleep, otherwise all subsequent
     * register reads return zero. */
    (void)bmm150_power_on(st);

    /* Step 2: first init -- reads chip_id and trim data. */
    rslt = bmm150_init(&st->dev);
    ESP_LOGI(TAG, "BMM150 init at 0x%02x -> rslt=%d, chip_id=0x%02x",
             i2c_addr, rslt, st->dev.chip_id);

    /* Step 3: chip_id mismatch -> soft reset and retry (soft_reset also
     * re-applies power-on internally). */
    if (st->dev.chip_id != BMM150_CHIP_ID) {
        (void)bmm150_soft_reset(&st->dev);
        lua_mag_delay_us(BMM150_START_UP_TIME);
        rslt = bmm150_init(&st->dev);
        ESP_LOGI(TAG, "BMM150 re-init at 0x%02x -> rslt=%d, chip_id=0x%02x",
                 i2c_addr, rslt, st->dev.chip_id);
        if (st->dev.chip_id != BMM150_CHIP_ID) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    if (rslt != BMM150_OK) {
        ESP_LOGW(TAG, "BMM150 init failed at 0x%02x: %d", i2c_addr, rslt);
        return ESP_FAIL;
    }

    ctx->chip_id = st->dev.chip_id;
    return apply_default_runtime_config(st);
}

static esp_err_t bmm150_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    bmm150_state_t *st = (bmm150_state_t *)ctx->state;
    struct bmm150_settings settings = { 0 };
    struct bmm150_mag_data data = { 0 };
    int8_t rslt;

    /* In FORCED mode the chip auto-returns to sleep after each conversion,
     * so we must retrigger FORCED before every read. */
    settings.pwr_mode = BMM150_POWERMODE_FORCED;
    rslt = bmm150_set_op_mode(&settings, &st->dev);
    if (rslt != BMM150_OK) {
        return ESP_FAIL;
    }

    rslt = bmm150_read_mag_data(&data, &st->dev);
    if (rslt != BMM150_OK) {
        return ESP_FAIL;
    }
    out->x = (float)data.x;
    out->y = (float)data.y;
    out->z = (float)data.z;
    out->temperature = 0.0f;
    rslt = bmm150_get_regs(BMM150_REG_INTERRUPT_STATUS, &out->status, 1, &st->dev);
    return (rslt == BMM150_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t bmm150_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    bmm150_state_t *st = (bmm150_state_t *)ctx->state;
    int8_t rslt = bmm150_get_regs(BMM150_REG_INTERRUPT_STATUS, out, 1, &st->dev);
    return (rslt == BMM150_OK) ? ESP_OK : ESP_FAIL;
}

static bool bmm150_is_supported_addr(uint8_t a)
{
    return a == BMM150_DEFAULT_I2C_ADDRESS ||
           a == BMM150_I2C_ADDRESS_CSB_LOW_SDO_HIGH ||
           a == BMM150_I2C_ADDRESS_CSB_HIGH_SDO_LOW ||
           a == BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH;
}

static uint8_t bmm150_default_addr(void)
{
    return BMM150_DEFAULT_I2C_ADDRESS;
}

static esp_err_t bmm150_probe_alternates(lua_mag_backend_ctx_t *ctx, uint8_t primary)
{
    static const uint8_t addrs[] = {
        BMM150_DEFAULT_I2C_ADDRESS,
        BMM150_I2C_ADDRESS_CSB_LOW_SDO_HIGH,
        BMM150_I2C_ADDRESS_CSB_HIGH_SDO_LOW,
        BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH,
    };

    for (size_t i = 0; i < sizeof(addrs); i++) {
        if (addrs[i] == primary) {
            continue;
        }
        ESP_LOGW(TAG, "Probe 0x%02x failed, retrying BMM150 at 0x%02x", primary, addrs[i]);
        esp_err_t err = bmm150_probe(ctx, addrs[i]);
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

const lua_mag_backend_t lua_mag_backend = {
    .chip_name = "bmm150",
    .state_size = sizeof(bmm150_state_t),
    .probe = bmm150_probe,
    .read_sample = bmm150_read_sample,
    .read_status = bmm150_read_status,
    .is_supported_addr = bmm150_is_supported_addr,
    .default_addr = bmm150_default_addr,
    .probe_alternates = bmm150_probe_alternates,
};

#endif /* CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM150 */
