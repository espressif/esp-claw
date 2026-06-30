/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "gen_board_device_custom.h"
#include "esp_io_expander.h"
#include "power_manager.h"

static const char *TAG = "CUSTOM_POWER_MANAGER";

esp_err_t cores3_power_manager_enable(void *device_handle, cores3_power_manager_feature_t feature)
{
    i2c_master_dev_handle_t axp2101_h = ((cores3_power_manager_handle_t *)device_handle)->pm_handle;
    esp_err_t err = ESP_OK;
    uint8_t data[2];
    esp_io_expander_handle_t *gpio_exp_aw9523 = NULL;

    err = esp_board_device_get_handle("gpio_expander", (void **)&gpio_exp_aw9523);
    switch (feature) {
        case CORES3_POWER_MANAGER_FEATURE_LCD:
            /* Enable LCD */
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1 << 9), 1);
            break;
        case CORES3_POWER_MANAGER_FEATURE_TOUCH:
            /* Enable Touch */
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1 << 0), 1);
            break;
        case CORES3_POWER_MANAGER_FEATURE_5V:
            /* Enable external 5V output (Grove / BUS_OUT):
             *   AW9523 P0_1 (pin 1)  = BUS_EN
             *   AW9523 P1_7 (pin 15) = BOOST_EN (5V boost converter)
             * Both must be high for the boost rail to appear on the port.
             */
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1u << 15), 1);
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1u << 1),  1);
            break;
        case CORES3_POWER_MANAGER_FEATURE_SD:
            /* AXP ALDO4 voltage / SD Card / 3V3 */
            data[0] = 0x95;
            data[1] = 0b00011100;  // 3V3
            err |= i2c_master_transmit(axp2101_h, data, sizeof(data), 1000);
            /* Enable SD */
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1 << 4), 1);
            break;
        case CORES3_POWER_MANAGER_FEATURE_SPEAKER:
            /* AXP ALDO1 voltage / PA PVDD / 1V8 */
            data[0] = 0x92;
            data[1] = 0b00001101;  // 1V8
            err |= i2c_master_transmit(axp2101_h, data, sizeof(data), 1000);
            /* AXP ALDO2 voltage / Codec / 3V3 */
            data[0] = 0x93;
            data[1] = 0b00011100;  // 3V3
            err |= i2c_master_transmit(axp2101_h, data, sizeof(data), 1000);
            /* AXP ALDO3 voltage / Codec+Mic / 3V3 */
            data[0] = 0x94;
            data[1] = 0b00011100;  // 3V3
            err |= i2c_master_transmit(axp2101_h, data, sizeof(data), 1000);
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1 << 2), 1);
            break;
        case CORES3_POWER_MANAGER_FEATURE_CAMERA:
            err |= esp_io_expander_set_level(*gpio_exp_aw9523, (1 << 8), 1);
            break;
        default:
            ESP_LOGE(TAG, "Unsupported feature");
            return ESP_ERR_INVALID_ARG;
    }

    return err;
}

int cores3_power_manager_init(void *config, int cfg_size, void **device_handle)
{
    ESP_LOGI(TAG, "Initializing power_manager device");
    dev_custom_axp2101_power_manager_config_t *power_manager_cfg = (dev_custom_axp2101_power_manager_config_t *)config;

    if (strcmp(power_manager_cfg->chip, "axp2101") != 0) {
        ESP_LOGE(TAG, "Unsupported power_manager chip: %s", power_manager_cfg->chip);
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t i2c_master_handle = NULL;
    esp_err_t err = esp_board_periph_get_handle(power_manager_cfg->peripheral_name, (void **)&i2c_master_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get i2c handle");
        return err;
    }

    cores3_power_manager_handle_t *handle = calloc(1, sizeof(cores3_power_manager_handle_t));
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to allocate power_manager handle");
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t axp2101_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = power_manager_cfg->i2c_addr,
        .scl_speed_hz = power_manager_cfg->frequency,
    };
    err = i2c_master_bus_add_device(i2c_master_handle, &axp2101_config, &handle->pm_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP2101 device to I2C bus");
        free(handle);
        return err;
    }

    cores3_power_manager_enable(handle, CORES3_POWER_MANAGER_FEATURE_SD);
    vTaskDelay(pdMS_TO_TICKS(100));
    cores3_power_manager_enable(handle, CORES3_POWER_MANAGER_FEATURE_SPEAKER);
    vTaskDelay(pdMS_TO_TICKS(100));
    cores3_power_manager_enable(handle, CORES3_POWER_MANAGER_FEATURE_LCD);
    vTaskDelay(pdMS_TO_TICKS(100));
    cores3_power_manager_enable(handle, CORES3_POWER_MANAGER_FEATURE_TOUCH);
    vTaskDelay(pdMS_TO_TICKS(100));
    cores3_power_manager_enable(handle, CORES3_POWER_MANAGER_FEATURE_5V);
    vTaskDelay(pdMS_TO_TICKS(100));

    const uint8_t lcd_bl_en[]  = {0x90, 0xBF};          // AXP DLDO1 Enable
    ESP_RETURN_ON_ERROR(i2c_master_transmit(handle->pm_handle, lcd_bl_en, sizeof(lcd_bl_en), 1000), TAG, "I2C write failed");
    const uint8_t lcd_bl_val[] = {0x99, 0b00011000};    // AXP DLDO1 voltage
    ESP_RETURN_ON_ERROR(i2c_master_transmit(handle->pm_handle, lcd_bl_val, sizeof(lcd_bl_val), 1000), TAG, "I2C write failed");

    *device_handle = handle;
    return ESP_OK;
}

int cores3_power_manager_deinit(void *device_handle)
{
    if (device_handle == NULL) {
        ESP_LOGW(TAG, "Power manager device handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    cores3_power_manager_handle_t *handle = (cores3_power_manager_handle_t *)device_handle;
    esp_err_t err = i2c_master_bus_rm_device(handle->pm_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remove AXP2101 device from I2C bus");
    }
    free(handle);
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(axp2101_power_manager, cores3_power_manager_init, cores3_power_manager_deinit);
