/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMC6309 backend for lua_module_magnetometer.
 *
 * No vendor SDK; talks to the chip via raw I2C register access.
 */

#include "sdkconfig.h"

#if CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_QMC6309

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#include "lua_module_mag_backend.h"
#include "qmc6309_regs.h"

static const char *TAG = "lua_mag_qmc6309";

static esp_err_t qmc_write_reg(lua_mag_backend_ctx_t *ctx, uint8_t reg, uint8_t val)
{
    return i2c_bus_write_bytes(ctx->i2c_dev_handle, reg, 1, &val);
}

static esp_err_t qmc6309_probe(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    ESP_RETURN_ON_ERROR(lua_mag_ctx_select_addr(ctx, i2c_addr), TAG,
                        "Failed to select QMC6309 I2C address 0x%02x", i2c_addr);

    ESP_RETURN_ON_ERROR(qmc_write_reg(ctx, QMC6309_REG_CTRL2, QMC6309_CTRL2_SOFT_RESET),
                        TAG, "QMC6309 soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(qmc_write_reg(ctx, QMC6309_REG_CTRL2, 0x00),
                        TAG, "QMC6309 clear reset failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(qmc_write_reg(ctx, QMC6309_REG_CTRL2, 0x00),
                        TAG, "QMC6309 range config failed");

    ESP_RETURN_ON_ERROR(qmc_write_reg(ctx, QMC6309_REG_CTRL1, QMC6309_CTRL1_CONT_MODE),
                        TAG, "QMC6309 continuous mode failed");

    uint8_t chip_id = 0;
    ESP_RETURN_ON_ERROR(i2c_bus_read_bytes(ctx->i2c_dev_handle, QMC6309_REG_CHIP_ID, 1, &chip_id),
                        TAG, "QMC6309 chip ID read failed");
    if (chip_id != QMC6309_CHIP_ID) {
        ESP_LOGE(TAG, "QMC6309 unexpected chip_id 0x%02x (expected 0x%02x)",
                 chip_id, QMC6309_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }
    ctx->chip_id = chip_id;

    ESP_LOGI(TAG, "QMC6309 configured, chip_id=0x%02X", chip_id);
    return ESP_OK;
}

static esp_err_t qmc6309_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    uint8_t status = 0;
    uint8_t data[6] = { 0 };

    do {
        esp_err_t err = i2c_bus_read_bytes(ctx->i2c_dev_handle, QMC6309_REG_STATUS, 1, &status);
        if (err != ESP_OK) {
            return err;
        }
    } while ((status & QMC6309_STATUS_DATA_READY) == 0);

    esp_err_t err = i2c_bus_read_bytes(ctx->i2c_dev_handle, QMC6309_REG_DATA_X_LSB,
                                       sizeof(data), data);
    if (err != ESP_OK) {
        return err;
    }

    out->x = (float)(int16_t)(data[0] | (data[1] << 8));
    out->y = (float)(int16_t)(data[2] | (data[3] << 8));
    out->z = (float)(int16_t)(data[4] | (data[5] << 8));
    out->temperature = 0.0f;
    out->status = status;
    return ESP_OK;
}

static esp_err_t qmc6309_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    return i2c_bus_read_bytes(ctx->i2c_dev_handle, QMC6309_REG_STATUS, 1, out);
}

static bool qmc6309_is_supported_addr(uint8_t a)
{
    return a == QMC6309_I2C_ADDR;
}

static uint8_t qmc6309_default_addr(void)
{
    return QMC6309_I2C_ADDR;
}

const lua_mag_backend_t lua_mag_backend = {
    .chip_name = "qmc6309",
    .state_size = 0,
    .probe = qmc6309_probe,
    .read_sample = qmc6309_read_sample,
    .read_status = qmc6309_read_status,
    .is_supported_addr = qmc6309_is_supported_addr,
    .default_addr = qmc6309_default_addr,
    .probe_alternates = NULL,
};

#endif /* CONFIG_LUA_MODULE_MAGNETOMETER_CHIP_QMC6309 */
