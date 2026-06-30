/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BMM350 backend for lua_module_magnetometer.
 */

#include "sdkconfig.h"

#if CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"

#include "bmm350.h"
#include "bmm350_defs.h"
#include "lua_module_mag_backend.h"

static const char *TAG = "lua_mag_bmm350";

typedef struct {
    struct bmm350_dev dev;
    lua_mag_backend_ctx_t *ctx;
} bmm350_state_t;

static BMM350_INTF_RET_TYPE bmm350_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                            uint32_t length, void *intf_ptr)
{
    bmm350_state_t *st = (bmm350_state_t *)intf_ptr;
    if (st == NULL || st->ctx == NULL || st->ctx->i2c_dev_handle == NULL) {
        return BMM350_E_COM_FAIL;
    }
    esp_err_t err = i2c_bus_read_bytes(st->ctx->i2c_dev_handle, reg_addr,
                                       (uint16_t)length, reg_data);
    return (err == ESP_OK) ? BMM350_INTF_RET_SUCCESS : BMM350_E_COM_FAIL;
}

static BMM350_INTF_RET_TYPE bmm350_i2c_write(uint8_t reg_addr, const uint8_t *reg_data,
                                             uint32_t length, void *intf_ptr)
{
    bmm350_state_t *st = (bmm350_state_t *)intf_ptr;
    if (st == NULL || st->ctx == NULL || st->ctx->i2c_dev_handle == NULL) {
        return BMM350_E_COM_FAIL;
    }
    esp_err_t err = i2c_bus_write_bytes(st->ctx->i2c_dev_handle, reg_addr,
                                        (uint16_t)length, reg_data);
    return (err == ESP_OK) ? BMM350_INTF_RET_SUCCESS : BMM350_E_COM_FAIL;
}

static void bmm350_delay_us_cb(uint32_t period_us, void *intf_ptr)
{
    (void)intf_ptr;
    lua_mag_delay_us(period_us);
}

static esp_err_t apply_default_runtime_config(bmm350_state_t *st)
{
    int8_t rslt;

    rslt = bmm350_set_odr_performance(BMM350_DATA_RATE_100HZ, BMM350_AVERAGING_4, &st->dev);
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "bmm350_set_odr_performance returned %d (continuing)", rslt);
    }

    rslt = bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN, &st->dev);
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "bmm350_enable_axes returned %d (continuing)", rslt);
    }

    rslt = bmm350_set_powermode(BMM350_NORMAL_MODE, &st->dev);
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "bmm350_set_powermode returned %d (continuing)", rslt);
    }

    uint8_t err_reg = 0;
    (void)bmm350_get_regs(BMM350_REG_ERR_REG, &err_reg, 1, &st->dev);
    ESP_LOGI(TAG, "BMM350 configured: ERR_REG=0x%02X", err_reg);
    return ESP_OK;
}

static esp_err_t bmm350_probe(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    bmm350_state_t *st = (bmm350_state_t *)ctx->state;
    int8_t rslt;

    ESP_RETURN_ON_ERROR(lua_mag_ctx_select_addr(ctx, i2c_addr), TAG,
                        "Failed to select BMM350 I2C address 0x%02x", i2c_addr);

    memset(&st->dev, 0, sizeof(st->dev));
    st->ctx = ctx;
    st->dev.read = bmm350_i2c_read;
    st->dev.write = bmm350_i2c_write;
    st->dev.delay_us = bmm350_delay_us_cb;
    st->dev.intf_ptr = st;

    rslt = bmm350_init(&st->dev);
    ESP_LOGI(TAG, "BMM350 init at 0x%02x -> rslt=%d, chip_id=0x%02x",
             i2c_addr, rslt, st->dev.chip_id);

    if (st->dev.chip_id != BMM350_CHIP_ID) {
        return ESP_ERR_NOT_FOUND;
    }
    if (rslt != BMM350_OK) {
        ESP_LOGW(TAG, "BMM350 init failed at 0x%02x: %d", i2c_addr, rslt);
        return ESP_FAIL;
    }

    ctx->chip_id = st->dev.chip_id;
    return apply_default_runtime_config(st);
}

static esp_err_t bmm350_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    bmm350_state_t *st = (bmm350_state_t *)ctx->state;
    struct bmm350_mag_temp_data data = { 0 };
    int8_t rslt = bmm350_get_compensated_mag_xyz_temp_data(&data, &st->dev);
    if (rslt != BMM350_OK) {
        return ESP_FAIL;
    }
    out->x = data.x;
    out->y = data.y;
    out->z = data.z;
    out->temperature = data.temperature;
    rslt = bmm350_get_regs(BMM350_REG_INT_STATUS, &out->status, 1, &st->dev);
    return (rslt == BMM350_OK) ? ESP_OK : ESP_FAIL;
}

static esp_err_t bmm350_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    bmm350_state_t *st = (bmm350_state_t *)ctx->state;
    int8_t rslt = bmm350_get_regs(BMM350_REG_INT_STATUS, out, 1, &st->dev);
    return (rslt == BMM350_OK) ? ESP_OK : ESP_FAIL;
}

static bool bmm350_is_supported_addr(uint8_t a)
{
    return a == BMM350_I2C_ADSEL_SET_LOW || a == BMM350_I2C_ADSEL_SET_HIGH;
}

static uint8_t bmm350_default_addr(void)
{
    return BMM350_I2C_ADSEL_SET_LOW;
}

static esp_err_t bmm350_probe_alternates(lua_mag_backend_ctx_t *ctx, uint8_t primary)
{
    const uint8_t alt = (primary == BMM350_I2C_ADSEL_SET_LOW) ?
                        BMM350_I2C_ADSEL_SET_HIGH : BMM350_I2C_ADSEL_SET_LOW;
    ESP_LOGW(TAG, "Probe 0x%02x failed, retrying BMM350 at 0x%02x", primary, alt);
    return bmm350_probe(ctx, alt);
}

const lua_mag_backend_t lua_mag_backend = {
    .chip_name = "bmm350",
    .state_size = sizeof(bmm350_state_t),
    .probe = bmm350_probe,
    .read_sample = bmm350_read_sample,
    .read_status = bmm350_read_status,
    .is_supported_addr = bmm350_is_supported_addr,
    .default_addr = bmm350_default_addr,
    .probe_alternates = bmm350_probe_alternates,
};

#endif /* CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_BMM350 */
