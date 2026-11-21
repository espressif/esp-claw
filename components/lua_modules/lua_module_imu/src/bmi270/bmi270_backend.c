/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BMI270 backend for lua_module_imu.
 */

#include <stdlib.h>

#include "bmi270_api.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.bmi270";

static esp_err_t bmi270_apply_default_runtime_config(bmi270_handle_t sensor_handle)
{
    uint8_t sens_list[2] = { BMI2_ACCEL, BMI2_GYRO };
    struct bmi2_sens_config config[2] = { 0 };
    config[BMI2_ACCEL].type = BMI2_ACCEL;
    config[BMI2_GYRO].type = BMI2_GYRO;

    int8_t rslt = bmi2_set_adv_power_save(BMI2_DISABLE, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to disable BMI270 APS mode: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi2_get_sensor_config(config, 2, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to get BMI270 sensor config: %d", rslt);
        return ESP_FAIL;
    }

    config[BMI2_ACCEL].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
    config[BMI2_ACCEL].cfg.acc.range = BMI2_ACC_RANGE_16G;
    config[BMI2_ACCEL].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[BMI2_ACCEL].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

    config[BMI2_GYRO].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
    config[BMI2_GYRO].cfg.gyr.range = BMI2_GYR_RANGE_2000;
    config[BMI2_GYRO].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[BMI2_GYRO].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    config[BMI2_GYRO].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    rslt = bmi2_set_sensor_config(config, 2, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to set BMI270 sensor config: %d", rslt);
        return ESP_FAIL;
    }

    rslt = bmi270_sensor_enable(sens_list, 2, sensor_handle);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to enable BMI270 accel/gyro: %d", rslt);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t bmi270_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    bmi270_handle_t sensor = NULL;
    if (!((i2c_addr == 0x68) || (i2c_addr == 0x69))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = bmi270_sensor_create_from_master_bus_with_address(ctx->master_bus, i2c_addr, &sensor,
                                         bmi270_config_file,
                                         BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    ctx->sensor = sensor;
    if (err != ESP_OK || ctx->sensor == NULL) {
        ESP_LOGE(TAG, "Failed to create BMI270 sensor: %s", esp_err_to_name(err));
        return err != ESP_OK ? err : ESP_FAIL;
    }

    struct bmi2_int_pin_config int_pin_cfg = { 0 };
    int_pin_cfg.pin_type = BMI2_INT1;
    int_pin_cfg.pin_cfg[0].input_en = BMI2_INT_INPUT_DISABLE;
    int_pin_cfg.pin_cfg[0].lvl = BMI2_INT_ACTIVE_HIGH;
    int_pin_cfg.pin_cfg[0].od = BMI2_INT_PUSH_PULL;
    int_pin_cfg.pin_cfg[0].output_en = BMI2_INT_OUTPUT_ENABLE;
    int_pin_cfg.int_latch = BMI2_INT_NON_LATCH;

    int8_t rslt = bmi2_set_int_pin_config(&int_pin_cfg, ctx->sensor);
    if (rslt != BMI2_OK) {
        ESP_LOGE(TAG, "Failed to configure BMI270 INT1 pin: %d", rslt);
        return ESP_FAIL;
    }

    err = bmi270_apply_default_runtime_config(ctx->sensor);
    if (err != ESP_OK) {
        return err;
    }

    ctx->i2c_addr = i2c_addr;
    return ESP_OK;
}

static esp_err_t bmi270_backend_destroy(lua_imu_backend_ctx_t *ctx)
{
    bmi270_handle_t sensor = ctx->sensor;
    esp_err_t err = sensor ? bmi270_sensor_del(&sensor) : ESP_OK;
    ctx->sensor = sensor;
    return err;
}

static esp_err_t bmi270_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    struct bmi2_sens_data data = { 0 };
    int8_t rslt = bmi2_get_sensor_data(&data, ctx->sensor);
    if (rslt != BMI2_OK) {
        return ESP_FAIL;
    }
    out->accel.x = data.acc.x;
    out->accel.y = data.acc.y;
    out->accel.z = data.acc.z;
    out->gyro.x = data.gyr.x;
    out->gyro.y = data.gyr.y;
    out->gyro.z = data.gyr.z;
    out->sens_time = (int64_t)data.sens_time;
    out->status = data.status;
    return ESP_OK;
}

static esp_err_t bmi270_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    int16_t temp = 0;
    int8_t rslt = bmi2_get_temperature_data(&temp, ctx->sensor);
    if (rslt != BMI2_OK) {
        return ESP_FAIL;
    }
    *out = temp;
    return ESP_OK;
}

static esp_err_t bmi270_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    uint16_t int_status = 0;
    int8_t rslt = bmi2_get_int_status(&int_status, ctx->sensor);
    if (rslt != BMI2_OK) {
        return ESP_FAIL;
    }
    *out = int_status;
    return ESP_OK;
}

static bool bmi270_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == 0x68 || i2c_addr == 0x69;
}

static uint8_t bmi270_backend_default_addr(void)
{
    return BMI270_I2C_ADDRESS;
}

static int bmi270_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    /* BMI270 address strap: low for 0x68, high for 0x69. */
    return (i2c_addr == 0x69) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend_bmi270 = {
    .chip_name = "bmi270",
    .state_size = 0,
    .probe = bmi270_backend_probe,
    .destroy = bmi270_backend_destroy,
    .read_sample = bmi270_backend_read_sample,
    .read_temperature = bmi270_backend_read_temperature,
    .read_int_status = bmi270_backend_read_int_status,
    .is_supported_addr = bmi270_backend_is_supported_addr,
    .default_addr = bmi270_backend_default_addr,
    .sdo_level_for_addr = bmi270_backend_sdo_level_for_addr,
};
