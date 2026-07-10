/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MPU6050 backend for lua_module_imu. Uses direct i2c_bus device access
 * via the helpers exposed by the main module.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "mpu6050.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.mpu6050";

typedef struct {
    mpu6050_dev_t sensor_handle;
    lua_imu_backend_ctx_t *owner_ctx; /* used by sensor I/O callbacks */
} mpu6050_state_t;

static int8_t mpu6050_iface_read(uint8_t reg_addr, uint8_t *data, uint32_t len, void *intf_ptr)
{
    mpu6050_state_t *st = (mpu6050_state_t *)intf_ptr;
    if (st == NULL || st->owner_ctx == NULL || st->owner_ctx->i2c_dev_handle == NULL) {
        return MPU6050_E_COM_FAIL;
    }
    return (i2c_bus_read_bytes(st->owner_ctx->i2c_dev_handle, reg_addr, len, data) == ESP_OK) ?
           MPU6050_OK : MPU6050_E_COM_FAIL;
}

static int8_t mpu6050_iface_write(uint8_t reg_addr, const uint8_t *data, uint32_t len, void *intf_ptr)
{
    mpu6050_state_t *st = (mpu6050_state_t *)intf_ptr;
    if (st == NULL || st->owner_ctx == NULL || st->owner_ctx->i2c_dev_handle == NULL) {
        return MPU6050_E_COM_FAIL;
    }
    return (i2c_bus_write_bytes(st->owner_ctx->i2c_dev_handle, reg_addr, len, data) == ESP_OK) ?
           MPU6050_OK : MPU6050_E_COM_FAIL;
}

static void mpu6050_iface_delay_ms(uint32_t period_ms, void *intf_ptr)
{
    (void)intf_ptr;
    if (period_ms == 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(period_ms));
}

static esp_err_t mpu6050_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    mpu6050_state_t *st = (mpu6050_state_t *)ctx->state;

    esp_err_t err = lua_imu_ctx_select_addr(ctx, i2c_addr);
    if (err != ESP_OK) {
        return err;
    }

    memset(&st->sensor_handle, 0, sizeof(st->sensor_handle));
    st->owner_ctx = ctx;
    st->sensor_handle.intf_ptr = st;
    st->sensor_handle.read = mpu6050_iface_read;
    st->sensor_handle.write = mpu6050_iface_write;
    st->sensor_handle.delay_ms = mpu6050_iface_delay_ms;

    int8_t rslt = mpu6050_init(&st->sensor_handle);
    if (rslt != MPU6050_OK) {
        ESP_LOGE(TAG, "Failed to initialize MPU6050 at 0x%02x: %d", i2c_addr, rslt);
        return (rslt == MPU6050_E_DEV_NOT_FOUND) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t mpu6050_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    mpu6050_state_t *st = (mpu6050_state_t *)ctx->state;
    mpu6050_raw_axes_t acc = { 0 };
    mpu6050_raw_axes_t gyro = { 0 };
    uint8_t int_status = 0;

    int8_t rslt = mpu6050_read_accel_gyro(&acc, &gyro, &st->sensor_handle);
    if (rslt != MPU6050_OK) {
        return ESP_FAIL;
    }
    rslt = mpu6050_get_int_status(&int_status, &st->sensor_handle);
    if (rslt != MPU6050_OK) {
        return ESP_FAIL;
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

static esp_err_t mpu6050_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    mpu6050_state_t *st = (mpu6050_state_t *)ctx->state;
    int16_t temp = 0;
    int8_t rslt = mpu6050_read_temperature_raw(&temp, &st->sensor_handle);
    if (rslt != MPU6050_OK) {
        return ESP_FAIL;
    }
    *out = temp;
    return ESP_OK;
}

static esp_err_t mpu6050_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    mpu6050_state_t *st = (mpu6050_state_t *)ctx->state;
    uint8_t int_status = 0;
    int8_t rslt = mpu6050_get_int_status(&int_status, &st->sensor_handle);
    if (rslt != MPU6050_OK) {
        return ESP_FAIL;
    }
    *out = int_status;
    return ESP_OK;
}

static bool mpu6050_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == MPU6050_I2C_ADDRESS_LOW || i2c_addr == MPU6050_I2C_ADDRESS_HIGH;
}

static uint8_t mpu6050_backend_default_addr(void)
{
    return MPU6050_I2C_ADDRESS_LOW;
}

static int mpu6050_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    /* MPU6050 7-bit addresses: 0x68 (AD0=GND), 0x69 (AD0=VDDIO). */
    return (i2c_addr == MPU6050_I2C_ADDRESS_HIGH) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend = {
    .chip_name = "mpu6050",
    .state_size = sizeof(mpu6050_state_t),
    .probe = mpu6050_backend_probe,
    .destroy = NULL,
    .read_sample = mpu6050_backend_read_sample,
    .read_temperature = mpu6050_backend_read_temperature,
    .read_int_status = mpu6050_backend_read_int_status,
    .is_supported_addr = mpu6050_backend_is_supported_addr,
    .default_addr = mpu6050_backend_default_addr,
    .sdo_level_for_addr = mpu6050_backend_sdo_level_for_addr,
};
