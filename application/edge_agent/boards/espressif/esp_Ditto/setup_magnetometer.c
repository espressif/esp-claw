/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include "bmm150_common.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "gen_board_device_custom.h"

static const char *TAG = "ditto_magnetometer";

static int magnetometer_sensor_init(void *config, int size, void **out)
{
    ESP_RETURN_ON_FALSE(config && out && size == sizeof(dev_custom_magnetometer_sensor_config_t), ESP_ERR_INVALID_ARG, TAG, "Invalid magnetometer config");
    *out = NULL;
    const dev_custom_magnetometer_sensor_config_t *cfg = config;
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_handle(cfg->peripheral_name, (void **)&bus), TAG, "I2C bus unavailable");
    struct bmm150_dev *sensor = calloc(1, sizeof(*sensor));
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_NO_MEM, TAG, "Sensor allocation failed");
    esp_err_t err = bmm150_interface_init_from_master_bus(sensor, bus, cfg->i2c_addr, cfg->frequency);
    if (err == ESP_OK && (bmm150_init(sensor) != BMM150_OK || sensor->chip_id != BMM150_CHIP_ID)) err = ESP_FAIL;
    if (err == ESP_OK) {
        struct bmm150_settings settings = {.pwr_mode = BMM150_POWERMODE_NORMAL, .preset_mode = BMM150_PRESETMODE_REGULAR};
        if (bmm150_set_presetmode(&settings, sensor) != BMM150_OK || bmm150_set_op_mode(&settings, sensor) != BMM150_OK) err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        if (sensor->intf_ptr) {
            esp_err_t cleanup = bmm150_interface_deinit_device(sensor);
            if (cleanup != ESP_OK) {
                ESP_LOGE(TAG, "Sensor cleanup failed: %s", esp_err_to_name(cleanup));
                return cleanup;
            }
        }
        free(sensor);
        ESP_LOGE(TAG, "Magnetometer init failed: %s", esp_err_to_name(err));
        return err;
    }
    *out = sensor;
    return ESP_OK;
}

static int magnetometer_sensor_deinit(void *handle)
{
    struct bmm150_dev *sensor = handle;
    esp_err_t err = bmm150_interface_deinit_device(sensor);
    if (err == ESP_OK) free(sensor);
    return err;
}

CUSTOM_DEVICE_IMPLEMENT(magnetometer_sensor, magnetometer_sensor_init, magnetometer_sensor_deinit);
