/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include "bmm350.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "gen_board_device_custom.h"

#define BMM350_TRANSFER_TIMEOUT_MS 100

static BMM350_INTF_RET_TYPE sensor_read(uint8_t reg, uint8_t *data, uint32_t length, void *device)
{
    return i2c_master_transmit_receive(device, &reg, 1, data, length, BMM350_TRANSFER_TIMEOUT_MS) == ESP_OK ? BMM350_INTF_RET_SUCCESS : BMM350_E_COM_FAIL;
}

static BMM350_INTF_RET_TYPE sensor_write(uint8_t reg, const uint8_t *data, uint32_t length, void *device)
{
    if (length > UINT16_MAX) return BMM350_E_COM_FAIL;
    uint8_t *buffer = malloc(length + 1);
    if (!buffer) return BMM350_E_COM_FAIL;
    buffer[0] = reg;
    memcpy(buffer + 1, data, length);
    esp_err_t err = i2c_master_transmit(device, buffer, length + 1, BMM350_TRANSFER_TIMEOUT_MS);
    free(buffer);
    return err == ESP_OK ? BMM350_INTF_RET_SUCCESS : BMM350_E_COM_FAIL;
}

static void sensor_delay(uint32_t us, void *context)
{
    (void)context;
    if (us < 1000) esp_rom_delay_us(us);
    else vTaskDelay(pdMS_TO_TICKS((us + 999) / 1000) + 1);
}

static const char *TAG = "shuttle_magnetometer";

static int magnetometer_sensor_init(void *config, int size, void **out)
{
    ESP_RETURN_ON_FALSE(config && out && size == sizeof(dev_custom_magnetometer_sensor_config_t), ESP_ERR_INVALID_ARG, TAG, "Invalid magnetometer config");
    *out = NULL;
    const dev_custom_magnetometer_sensor_config_t *cfg = config;
    ESP_RETURN_ON_FALSE(cfg->i2c_addr == BMM350_I2C_ADSEL_SET_LOW || cfg->i2c_addr == BMM350_I2C_ADSEL_SET_HIGH, ESP_ERR_INVALID_ARG, TAG, "Invalid address");
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_handle(cfg->peripheral_name, (void **)&bus), TAG, "I2C bus unavailable");
    if (cfg->sdo_gpio_num >= 0) {
        ESP_RETURN_ON_FALSE(GPIO_IS_VALID_OUTPUT_GPIO(cfg->sdo_gpio_num), ESP_ERR_INVALID_ARG, TAG, "Invalid address strap GPIO");
        const gpio_config_t pin = {.pin_bit_mask = 1ULL << cfg->sdo_gpio_num, .mode = GPIO_MODE_OUTPUT};
        ESP_RETURN_ON_ERROR(gpio_config(&pin), TAG, "Address strap config failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(cfg->sdo_gpio_num, cfg->i2c_addr == BMM350_I2C_ADSEL_SET_HIGH), TAG, "Address strap level failed");
    }
    struct bmm350_dev *sensor = calloc(1, sizeof(*sensor));
    ESP_RETURN_ON_FALSE(sensor, ESP_ERR_NO_MEM, TAG, "Sensor allocation failed");
    i2c_master_dev_handle_t device = NULL;
    i2c_device_config_t dev_cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = cfg->i2c_addr, .scl_speed_hz = cfg->frequency};
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &device);
    sensor->intf_ptr = device;
    sensor->read = sensor_read;
    sensor->write = sensor_write;
    sensor->delay_us = sensor_delay;
    if (err == ESP_OK && (bmm350_init(sensor) != BMM350_OK || sensor->chip_id != BMM350_CHIP_ID ||
        bmm350_set_odr_performance(BMM350_DATA_RATE_100HZ, BMM350_AVERAGING_4, sensor) != BMM350_OK ||
        bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN, sensor) != BMM350_OK ||
        bmm350_set_powermode(BMM350_NORMAL_MODE, sensor) != BMM350_OK)) err = ESP_FAIL;
    if (err != ESP_OK) {
        if (device) {
            esp_err_t cleanup = i2c_master_bus_rm_device(device);
            if (cleanup != ESP_OK) {
                ESP_LOGE(TAG, "Sensor cleanup failed: %s", esp_err_to_name(cleanup));
                free(sensor);
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
    struct bmm350_dev *sensor = handle;
    esp_err_t err = i2c_master_bus_rm_device(sensor->intf_ptr);
    if (err == ESP_OK) free(sensor);
    return err;
}

CUSTOM_DEVICE_IMPLEMENT(magnetometer_sensor, magnetometer_sensor_init, magnetometer_sensor_deinit);
