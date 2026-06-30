/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ICM42670 backend for lua_module_imu.
 */

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_bus.h"
#include "icm42670.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.icm42670";

typedef struct {
    icm42670_handle_t sensor_handle;
} icm42670_state_t;

static esp_err_t icm42670_apply_default_runtime_config(icm42670_handle_t sensor_handle)
{
    const icm42670_cfg_t imu_cfg = {
        .acce_fs = ACCE_FS_16G,
        .acce_odr = ACCE_ODR_200HZ,
        .gyro_fs = GYRO_FS_2000DPS,
        .gyro_odr = GYRO_ODR_200HZ,
    };
    ESP_RETURN_ON_ERROR(icm42670_config(sensor_handle, &imu_cfg), TAG,
                        "Failed to configure ICM42670 accel/gyro");
    ESP_RETURN_ON_ERROR(icm42670_acce_set_pwr(sensor_handle, ACCE_PWR_LOWNOISE), TAG,
                        "Failed to enable ICM42670 accelerometer");
    ESP_RETURN_ON_ERROR(icm42670_gyro_set_pwr(sensor_handle, GYRO_PWR_LOWNOISE), TAG,
                        "Failed to enable ICM42670 gyroscope");
    return ESP_OK;
}

static esp_err_t icm42670_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    icm42670_state_t *st = (icm42670_state_t *)ctx->state;

    i2c_master_bus_handle_t i2c_master_handle = i2c_bus_get_internal_bus_handle(ctx->i2c_bus_handle);
    if (i2c_master_handle == NULL) {
        ESP_LOGE(TAG, "Failed to resolve internal I2C handle");
        return ESP_FAIL;
    }

    esp_err_t err = icm42670_create(i2c_master_handle, i2c_addr, &st->sensor_handle);
    if (err != ESP_OK || st->sensor_handle == NULL) {
        ESP_LOGE(TAG, "Failed to create ICM42670 sensor: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    err = icm42670_apply_default_runtime_config(st->sensor_handle);
    if (err != ESP_OK) {
        icm42670_delete(st->sensor_handle);
        st->sensor_handle = NULL;
        return err;
    }
    ctx->i2c_addr = i2c_addr;
    return ESP_OK;
}

static void icm42670_backend_destroy(lua_imu_backend_ctx_t *ctx)
{
    icm42670_state_t *st = (icm42670_state_t *)ctx->state;
    if (st->sensor_handle != NULL) {
        icm42670_delete(st->sensor_handle);
        st->sensor_handle = NULL;
    }
}

static esp_err_t icm42670_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    icm42670_state_t *st = (icm42670_state_t *)ctx->state;
    icm42670_raw_value_t acc = { 0 };
    icm42670_raw_value_t gyro = { 0 };
    uint8_t int_status = 0;

    esp_err_t err = icm42670_get_acce_raw_value(st->sensor_handle, &acc);
    if (err == ESP_OK) {
        err = icm42670_get_gyro_raw_value(st->sensor_handle, &gyro);
    }
    if (err == ESP_OK) {
        err = icm42670_read_register(st->sensor_handle, ICM42670_INT_STATUS, &int_status);
    }
    if (err != ESP_OK) {
        return err;
    }
    out->accel.x = acc.x;
    out->accel.y = acc.y;
    out->accel.z = acc.z;
    out->gyro.x = gyro.x;
    out->gyro.y = gyro.y;
    out->gyro.z = gyro.z;
    out->sens_time = esp_timer_get_time();
    out->status = int_status;
    return ESP_OK;
}

static esp_err_t icm42670_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    icm42670_state_t *st = (icm42670_state_t *)ctx->state;
    uint16_t temp = 0;
    esp_err_t err = icm42670_get_temp_raw_value(st->sensor_handle, &temp);
    if (err != ESP_OK) {
        return err;
    }
    *out = temp;
    return ESP_OK;
}

static esp_err_t icm42670_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    icm42670_state_t *st = (icm42670_state_t *)ctx->state;
    uint8_t s0 = 0, s2 = 0, s3 = 0;
    esp_err_t err = icm42670_read_register(st->sensor_handle, ICM42670_INT_STATUS, &s0);
    if (err == ESP_OK) {
        err = icm42670_read_register(st->sensor_handle, ICM42670_INT_STATUS2, &s2);
    }
    if (err == ESP_OK) {
        err = icm42670_read_register(st->sensor_handle, ICM42670_INT_STATUS3, &s3);
    }
    if (err != ESP_OK) {
        return err;
    }
    *out = ((uint32_t)s3 << 16) | ((uint32_t)s2 << 8) | s0;
    return ESP_OK;
}

static bool icm42670_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == ICM42670_I2C_ADDRESS || i2c_addr == ICM42670_I2C_ADDRESS_1;
}

static uint8_t icm42670_backend_default_addr(void)
{
    return ICM42670_I2C_ADDRESS;
}

static int icm42670_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    /* ICM42670 7-bit addresses: 0x68 (AP_AD0=GND), 0x69 (AP_AD0=VDDIO). */
    return (i2c_addr == ICM42670_I2C_ADDRESS_1) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend = {
    .chip_name = "icm42670",
    .state_size = sizeof(icm42670_state_t),
    .probe = icm42670_backend_probe,
    .destroy = icm42670_backend_destroy,
    .read_sample = icm42670_backend_read_sample,
    .read_temperature = icm42670_backend_read_temperature,
    .read_int_status = icm42670_backend_read_int_status,
    .is_supported_addr = icm42670_backend_is_supported_addr,
    .default_addr = icm42670_backend_default_addr,
    .sdo_level_for_addr = icm42670_backend_sdo_level_for_addr,
};
