/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_module_mag_backend.h"
#include "qmc6309_regs.h"

#define QMC6309_TIMEOUT_MS 100

/* QMC6309 board adapters expose a native I2C device handle. */
static esp_err_t qmc_read(lua_mag_backend_ctx_t *ctx, uint8_t reg, uint8_t *data, size_t size)
{
    return i2c_master_transmit_receive(ctx->sensor, &reg, 1, data, size, QMC6309_TIMEOUT_MS);
}

static esp_err_t qmc_write(lua_mag_backend_ctx_t *ctx, uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    return i2c_master_transmit(ctx->sensor, data, sizeof(data), QMC6309_TIMEOUT_MS);
}

static esp_err_t qmc6309_create(lua_mag_backend_ctx_t *ctx, uint8_t address)
{
    i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = ctx->frequency};
    esp_err_t err = i2c_master_bus_add_device(ctx->master_bus, &config, &ctx->i2c_dev_handle);
    if (err != ESP_OK) return err;
    ctx->sensor = ctx->i2c_dev_handle;
    uint8_t id;
    err = qmc_read(ctx, QMC6309_REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) return err;
    if (id != QMC6309_CHIP_ID) return ESP_ERR_NOT_FOUND;
    err = qmc_write(ctx, QMC6309_REG_CTRL2, QMC6309_CTRL2_SOFT_RESET);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    err = qmc_write(ctx, QMC6309_REG_CTRL2, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    return qmc_write(ctx, QMC6309_REG_CTRL1, QMC6309_CTRL1_CONT_MODE);
}

static esp_err_t qmc6309_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    const int64_t deadline = esp_timer_get_time() + QMC6309_TIMEOUT_MS * 1000;
    while (true) {
        esp_err_t err = qmc_read(ctx, QMC6309_REG_STATUS, &out->status, 1);
        if (err != ESP_OK) return err;
        if (out->status & QMC6309_STATUS_DATA_READY) break;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    uint8_t data[6];
    esp_err_t err = qmc_read(ctx, QMC6309_REG_DATA_X_LSB, data, sizeof(data));
    if (err != ESP_OK) return err;
    out->x = (int16_t)(data[0] | (data[1] << 8));
    out->y = (int16_t)(data[2] | (data[3] << 8));
    out->z = (int16_t)(data[4] | (data[5] << 8));
    return ESP_OK;
}

static esp_err_t qmc6309_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    return qmc_read(ctx, QMC6309_REG_STATUS, out, 1);
}

static bool qmc6309_is_supported_addr(uint8_t address)
{
    return address == QMC6309_I2C_ADDR;
}

static uint8_t qmc6309_default_addr(void)
{
    return QMC6309_I2C_ADDR;
}

const lua_mag_backend_t lua_mag_backend_qmc6309 = {
    .chip_id = QMC6309_CHIP_ID,
    .create = qmc6309_create,
    .read_sample = qmc6309_read_sample, .read_status = qmc6309_read_status,
    .is_supported_addr = qmc6309_is_supported_addr, .default_addr = qmc6309_default_addr,
};
