/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * QMI8658 6-axis IMU backend for lua_module_imu.
 *
 * Self-contained register-level driver using the shared i2c_bus device handle.
 * Register map / init sequence follow the QMI8658 datasheet.
 */

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

#include "lua_module_imu_backend.h"

static const char *TAG = "lua_module_imu.qmi8658";

#define QMI8658_REG_WHO_AM_I 0x00
#define QMI8658_REG_CTRL1    0x02
#define QMI8658_REG_CTRL2    0x03
#define QMI8658_REG_CTRL3    0x04
#define QMI8658_REG_CTRL7    0x08
#define QMI8658_REG_STATUS0  0x2E
#define QMI8658_REG_TEMP_L   0x33
#define QMI8658_REG_AX_L     0x35
#define QMI8658_REG_RESET    0x60

#define QMI8658_CHIP_ID      0x05

#define QMI8658_I2C_ADDRESS   0x6A
#define QMI8658_I2C_ADDRESS_1 0x6B

/* CTRL1: address auto-increment enabled */
#define QMI8658_CTRL1_VALUE   0x40
/* CTRL7: enable accelerometer + gyroscope */
#define QMI8658_CTRL7_VALUE   0x03
/* CTRL2: accel +/-4g, 250 Hz */
#define QMI8658_CTRL2_VALUE   0x95
/* CTRL3: gyro +/-512 dps, 250 Hz */
#define QMI8658_CTRL3_VALUE   0xD5
#define QMI8658_RESET_VALUE   0xB0

#define QMI8658_AXES_BYTES    12

static esp_err_t qmi8658_write_reg(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t value)
{
    if (ctx->i2c_dev_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_bus_write_bytes(ctx->i2c_dev_handle, reg, 1, &value);
}

static esp_err_t qmi8658_read_regs(lua_imu_backend_ctx_t *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    if (ctx->i2c_dev_handle == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_bus_read_bytes(ctx->i2c_dev_handle, reg, len, data);
}

static int16_t qmi8658_read_le16(const uint8_t *buf)
{
    return (int16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
}

static esp_err_t qmi8658_backend_probe(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr)
{
    esp_err_t err = lua_imu_ctx_select_addr(ctx, i2c_addr);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t chip_id = 0;
    err = qmi8658_read_regs(ctx, QMI8658_REG_WHO_AM_I, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read WHO_AM_I at 0x%02x: %s", i2c_addr, esp_err_to_name(err));
        return err;
    }
    if (chip_id != QMI8658_CHIP_ID) {
        ESP_LOGE(TAG, "Unexpected QMI8658 chip id 0x%02x", chip_id);
        return ESP_ERR_NOT_FOUND;
    }

    err = qmi8658_write_reg(ctx, QMI8658_REG_RESET, QMI8658_RESET_VALUE);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    err = qmi8658_write_reg(ctx, QMI8658_REG_CTRL1, QMI8658_CTRL1_VALUE);
    if (err == ESP_OK) {
        err = qmi8658_write_reg(ctx, QMI8658_REG_CTRL7, QMI8658_CTRL7_VALUE);
    }
    if (err == ESP_OK) {
        err = qmi8658_write_reg(ctx, QMI8658_REG_CTRL2, QMI8658_CTRL2_VALUE);
    }
    if (err == ESP_OK) {
        err = qmi8658_write_reg(ctx, QMI8658_REG_CTRL3, QMI8658_CTRL3_VALUE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure QMI8658: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "QMI8658 ready at 0x%02x (accel 4g/250Hz, gyro 512dps/250Hz)", i2c_addr);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_sample(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out)
{
    uint8_t status = 0;
    uint8_t raw[QMI8658_AXES_BYTES] = { 0 };

    esp_err_t err = qmi8658_read_regs(ctx, QMI8658_REG_STATUS0, &status, 1);
    if (err == ESP_OK) {
        err = qmi8658_read_regs(ctx, QMI8658_REG_AX_L, raw, sizeof(raw));
    }
    if (err != ESP_OK) {
        return err;
    }

    out->accel.x = qmi8658_read_le16(&raw[0]);
    out->accel.y = qmi8658_read_le16(&raw[2]);
    out->accel.z = qmi8658_read_le16(&raw[4]);
    out->gyro.x = qmi8658_read_le16(&raw[6]);
    out->gyro.y = qmi8658_read_le16(&raw[8]);
    out->gyro.z = qmi8658_read_le16(&raw[10]);
    out->sens_time = esp_timer_get_time();
    out->status = status;
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_temperature(lua_imu_backend_ctx_t *ctx, int32_t *out)
{
    uint8_t raw[2] = { 0 };
    esp_err_t err = qmi8658_read_regs(ctx, QMI8658_REG_TEMP_L, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }
    *out = qmi8658_read_le16(raw);
    return ESP_OK;
}

static esp_err_t qmi8658_backend_read_int_status(lua_imu_backend_ctx_t *ctx, uint32_t *out)
{
    uint8_t status = 0;
    esp_err_t err = qmi8658_read_regs(ctx, QMI8658_REG_STATUS0, &status, 1);
    if (err != ESP_OK) {
        return err;
    }
    *out = status;
    return ESP_OK;
}

static bool qmi8658_backend_is_supported_addr(uint8_t i2c_addr)
{
    return i2c_addr == QMI8658_I2C_ADDRESS || i2c_addr == QMI8658_I2C_ADDRESS_1;
}

static uint8_t qmi8658_backend_default_addr(void)
{
    return QMI8658_I2C_ADDRESS;
}

static int qmi8658_backend_sdo_level_for_addr(uint8_t i2c_addr)
{
    /* QMI8658 7-bit addresses: 0x6A (SA0=1), 0x6B (SA0=0). */
    return (i2c_addr == QMI8658_I2C_ADDRESS) ? 1 : 0;
}

const lua_imu_backend_t lua_imu_backend = {
    .chip_name = "qmi8658",
    .state_size = 0,
    .probe = qmi8658_backend_probe,
    .destroy = NULL,
    .read_sample = qmi8658_backend_read_sample,
    .read_temperature = qmi8658_backend_read_temperature,
    .read_int_status = qmi8658_backend_read_int_status,
    .is_supported_addr = qmi8658_backend_is_supported_addr,
    .default_addr = qmi8658_backend_default_addr,
    .sdo_level_for_addr = qmi8658_backend_sdo_level_for_addr,
};
