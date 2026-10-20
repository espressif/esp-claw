/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "bmi270_api.h"
#include "driver/gpio.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "gen_board_device_custom.h"

static const char *TAG = "vocat_imu";

static int imu_sensor_init(void *config, int size, void **out)
{
    ESP_RETURN_ON_FALSE(config && out && size == sizeof(dev_custom_imu_sensor_config_t), ESP_ERR_INVALID_ARG, TAG, "Invalid IMU config");
    *out = NULL;
    const dev_custom_imu_sensor_config_t *cfg = config;
    ESP_RETURN_ON_FALSE(cfg->int_gpio_num < 0 || GPIO_IS_VALID_GPIO(cfg->int_gpio_num), ESP_ERR_INVALID_ARG, TAG, "Invalid interrupt GPIO");
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_handle(cfg->peripheral_name, (void **)&bus), TAG, "I2C bus unavailable");
    bmi270_handle_t sensor = NULL;
    esp_err_t err = bmi270_sensor_create_from_master_bus_with_address(bus, cfg->i2c_addr, &sensor,
        bmi270_config_file, BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (err == ESP_OK && cfg->int_gpio_num >= 0) {
        const gpio_config_t pin = {.pin_bit_mask = 1ULL << cfg->int_gpio_num, .mode = GPIO_MODE_INPUT};
        err = gpio_config(&pin);
    }
    if (err == ESP_OK) {
        /* Preserve the sampling configuration previously used by Lua. */
        struct bmi2_sens_config settings[2] = {{.type = BMI2_ACCEL}, {.type = BMI2_GYRO}};
        const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
        settings[0].cfg.acc.odr = BMI2_ACC_ODR_200HZ;
        settings[0].cfg.acc.range = BMI2_ACC_RANGE_16G;
        settings[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
        settings[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
        settings[1].cfg.gyr.odr = BMI2_GYR_ODR_200HZ;
        settings[1].cfg.gyr.range = BMI2_GYR_RANGE_2000;
        settings[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
        settings[1].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
        settings[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
        if (bmi2_set_adv_power_save(BMI2_DISABLE, sensor) != BMI2_OK ||
            bmi2_set_sensor_config(settings, 2, sensor) != BMI2_OK ||
            bmi270_sensor_enable(sensors, 2, sensor) != BMI2_OK) err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        if (sensor) (void)bmi270_sensor_del(&sensor);
        if (cfg->int_gpio_num >= 0) (void)gpio_reset_pin(cfg->int_gpio_num);
        ESP_LOGE(TAG, "IMU init failed: %s", esp_err_to_name(err));
        return err;
    }
    *out = sensor;
    return ESP_OK;
}

static int imu_sensor_deinit(void *handle)
{
    bmi270_handle_t sensor = handle;
    return bmi270_sensor_del(&sensor);
}

CUSTOM_DEVICE_IMPLEMENT(imu_sensor, imu_sensor_init, imu_sensor_deinit);
