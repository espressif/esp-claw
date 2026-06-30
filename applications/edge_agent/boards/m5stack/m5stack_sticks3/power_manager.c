/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * M5Stack StickS3 power manager — drives the on-board M5PM1 PMIC over I2C.
 *
 * The M5PM1 register sequence is taken from the M5GFX
 * `board_M5StickS3` initialisation block:
 *   - Reg 0x09: I2C_CFG (write 0x00 to disable idle sleep)
 *   - Reg 0x10: GPIO direction (1 = output)
 *   - Reg 0x11: GPIO output level
 *   - Reg 0x13: GPIO drive mode (0 = push-pull)
 *   - Reg 0x16: GPIO function selector (0 = generic GPIO)
 *
 * Channel map:
 *   - PYG2 (bit 2): LCD / L3B power enable
 *   - PYG3 (bit 3): Speaker amplifier enable
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "dev_custom.h"
#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "esp_board_entry.h"
#include "gen_board_device_custom.h"
#include "power_manager.h"

#define M5PM1_REG_I2C_CFG     0x09
#define M5PM1_REG_DIRECTION   0x10
#define M5PM1_REG_OUTPUT      0x11
#define M5PM1_REG_DRIVE_MODE  0x13
#define M5PM1_REG_FUNC_SEL    0x16

#define M5PM1_BIT_LCD_PWR     (1 << 2)
#define M5PM1_BIT_SPK_PA      (1 << 3)

static const char *TAG = "M5_STICKS3_PM";

typedef struct {
    i2c_master_bus_handle_t  bus;
    i2c_master_dev_handle_t  dev;
    const char              *peripheral_name;
} m5_sticks3_pm_handle_t;

static m5_sticks3_pm_handle_t *s_handle;

static esp_err_t m5pm1_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_handle->dev, &reg, 1, value, 1, 200 / portTICK_PERIOD_MS);
}

static esp_err_t m5pm1_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_handle->dev, buf, sizeof(buf), 200 / portTICK_PERIOD_MS);
}

static esp_err_t m5pm1_update_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t cur = 0;
    esp_err_t err = m5pm1_read_reg(reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t next = (cur & ~mask) | (value & mask);
    if (next == cur) {
        return ESP_OK;
    }
    return m5pm1_write_reg(reg, next);
}

static esp_err_t m5pm1_configure_output(uint8_t bit_mask, bool initial_high)
{
    /* Function = generic GPIO */
    ESP_RETURN_ON_ERROR(m5pm1_update_bits(M5PM1_REG_FUNC_SEL,    bit_mask, 0),       TAG, "func_sel");
    /* Drive mode = push-pull */
    ESP_RETURN_ON_ERROR(m5pm1_update_bits(M5PM1_REG_DRIVE_MODE,  bit_mask, 0),       TAG, "drive_mode");
    /* Direction = output */
    ESP_RETURN_ON_ERROR(m5pm1_update_bits(M5PM1_REG_DIRECTION,   bit_mask, bit_mask), TAG, "direction");
    /* Initial level */
    ESP_RETURN_ON_ERROR(m5pm1_update_bits(M5PM1_REG_OUTPUT, bit_mask,
                                          initial_high ? bit_mask : 0), TAG, "output");
    return ESP_OK;
}

static uint8_t feature_bit(m5_sticks3_power_feature_t feature)
{
    switch (feature) {
    case M5_STICKS3_PWR_LCD:     return M5PM1_BIT_LCD_PWR;
    case M5_STICKS3_PWR_SPEAKER: return M5PM1_BIT_SPK_PA;
    default:                     return 0;
    }
}

esp_err_t m5_sticks3_power_enable(m5_sticks3_power_feature_t feature, bool enable)
{
    if (s_handle == NULL) {
        ESP_LOGE(TAG, "Power manager not initialised");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t mask = feature_bit(feature);
    if (mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return m5pm1_update_bits(M5PM1_REG_OUTPUT, mask, enable ? mask : 0);
}

static int m5_sticks3_pm_init(void *config, int cfg_size, void **device_handle)
{
    (void)cfg_size;
    if (config == NULL || device_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_handle != NULL) {
        *device_handle = s_handle;
        return ESP_OK;
    }

    const dev_custom_sticks3_power_manager_config_t *cfg =
        (const dev_custom_sticks3_power_manager_config_t *)config;

    if (cfg->chip == NULL || strcmp(cfg->chip, "m5pm1") != 0) {
        ESP_LOGE(TAG, "Unsupported power_manager chip: %s", cfg->chip ? cfg->chip : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = esp_board_periph_get_handle(cfg->peripheral_name, (void **)&bus);
    if (err != ESP_OK || bus == NULL) {
        ESP_LOGE(TAG, "Failed to get I2C bus '%s': %d", cfg->peripheral_name, err);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    m5_sticks3_pm_handle_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    handle->bus = bus;
    handle->peripheral_name = cfg->peripheral_name;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = (uint16_t)cfg->i2c_address,
        .scl_speed_hz    = (uint32_t)cfg->i2c_frequency,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &handle->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        free(handle);
        return err;
    }

    s_handle = handle;

    /* Disable PMIC idle sleep so we can drive it reliably from the SoC. */
    err = m5pm1_write_reg(M5PM1_REG_I2C_CFG, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write I2C_CFG: %s", esp_err_to_name(err));
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Bring up LCD power and speaker PA — display_lcd / audio_dac depend on these. */
    err = m5pm1_configure_output(M5PM1_BIT_LCD_PWR, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable LCD rail: %s", esp_err_to_name(err));
        goto fail;
    }
    err = m5pm1_configure_output(M5PM1_BIT_SPK_PA, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable speaker PA: %s", esp_err_to_name(err));
        goto fail;
    }

    *device_handle = handle;
    ESP_LOGI(TAG, "M5PM1 power manager initialised (LCD + SPK rails on)");
    return ESP_OK;

fail:
    if (handle->dev) {
        i2c_master_bus_rm_device(handle->dev);
    }
    free(handle);
    s_handle = NULL;
    return err;
}

static int m5_sticks3_pm_deinit(void *device_handle)
{
    m5_sticks3_pm_handle_t *handle = (m5_sticks3_pm_handle_t *)device_handle;
    if (handle == NULL) {
        return ESP_OK;
    }
    if (handle->dev) {
        i2c_master_bus_rm_device(handle->dev);
    }
    if (handle->peripheral_name) {
        esp_board_periph_unref_handle(handle->peripheral_name);
    }
    free(handle);
    if (s_handle == handle) {
        s_handle = NULL;
    }
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(sticks3_power_manager, m5_sticks3_pm_init, m5_sticks3_pm_deinit);
