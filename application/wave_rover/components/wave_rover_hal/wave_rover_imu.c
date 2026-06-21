/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wr_imu";
static i2c_master_dev_handle_t s_qmi_dev  = NULL;
static i2c_master_dev_handle_t s_ak09_dev = NULL;

/* QMI8658 registers */
#define QMI_WHO_AM_I  0x00
#define QMI_CTRL1     0x02
#define QMI_CTRL2     0x03
#define QMI_CTRL3     0x04
#define QMI_CTRL7     0x08
#define QMI_TEMP_L    0x33
#define QMI_ACCX_L    0x35
#define QMI_GYRX_L    0x3B

/* AK09918 registers */
#define AK_ST1    0x10
#define AK_HXL    0x11
#define AK_CNTL2  0x31

/* AK09918 CNTL2 mode values */
#define AK_CNTL2_MODE_10HZ   0x01  /* continuous measurement mode 1 */
#define AK_CNTL2_POWER_DOWN  0x00

static esp_err_t qmi_read(uint8_t reg, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_qmi_dev, &reg, 1, 50), TAG, "qmi tx");
    return i2c_master_receive(s_qmi_dev, buf, len, 50);
}

static esp_err_t qmi_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_qmi_dev, buf, 2, 50);
}

static esp_err_t ak_read(uint8_t reg, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_ak09_dev, &reg, 1, 50), TAG, "ak tx");
    return i2c_master_receive(s_ak09_dev, buf, len, 50);
}

static esp_err_t ak_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_ak09_dev, buf, 2, 50);
}

static esp_err_t imu_hw_init(void)
{
    if (!g_wr_i2c_bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t qmi_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_QMI8658_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_wr_i2c_bus, &qmi_cfg, &s_qmi_dev),
                        TAG, "add qmi");

    uint8_t who;
    ESP_RETURN_ON_ERROR(qmi_read(QMI_WHO_AM_I, &who, 1), TAG, "who_am_i");
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I=0x%02X", who);

    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL1, 0x40), TAG, "ctrl1");
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL2, 0x03), TAG, "ctrl2"); /* accel: 4g */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL3, 0x55), TAG, "ctrl3"); /* gyro: 512dps */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL7, 0x03), TAG, "ctrl7"); /* enable acc+gyr */

    i2c_device_config_t ak_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_AK09918_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(g_wr_i2c_bus, &ak_cfg, &s_ak09_dev) == ESP_OK) {
        ak_write(AK_CNTL2, AK_CNTL2_MODE_10HZ); /* start in 10 Hz continuous mode */
        ESP_LOGI(TAG, "AK09918 initialized");
    } else {
        ESP_LOGW(TAG, "AK09918 not found, mag disabled");
    }
    return ESP_OK;
}

/* Gyro bias offsets applied in wr_imu_get_sample() */
static float s_gyro_off_x = 0.0f;
static float s_gyro_off_y = 0.0f;
static float s_gyro_off_z = 0.0f;

static float raw16_to_float(uint8_t lo, uint8_t hi, float scale)
{
    int16_t raw = (int16_t)(((uint16_t)hi << 8) | lo);
    return raw * scale;
}

esp_err_t wr_imu_get_sample(wr_imu_sample_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));

    if (!g_wr_i2c_bus) {
        s->present = false;
        s->accel_z = 1.0f;
        return ESP_OK;
    }

    if (!s_qmi_dev) {
        if (imu_hw_init() != ESP_OK) return ESP_OK;
    }

    uint8_t raw[12];
    if (qmi_read(QMI_ACCX_L, raw, 12) != ESP_OK) return ESP_OK;

    s->present = true;
    s->accel_x = raw16_to_float(raw[0],  raw[1],  4.0f / 32768.0f);
    s->accel_y = raw16_to_float(raw[2],  raw[3],  4.0f / 32768.0f);
    s->accel_z = raw16_to_float(raw[4],  raw[5],  4.0f / 32768.0f);
    s->gyro_x  = raw16_to_float(raw[6],  raw[7],  512.0f / 32768.0f) - s_gyro_off_x;
    s->gyro_y  = raw16_to_float(raw[8],  raw[9],  512.0f / 32768.0f) - s_gyro_off_y;
    s->gyro_z  = raw16_to_float(raw[10], raw[11], 512.0f / 32768.0f) - s_gyro_off_z;

    uint8_t tmp[2];
    if (qmi_read(QMI_TEMP_L, tmp, 2) == ESP_OK) {
        s->has_temperature = true;
        s->temperature_c   = raw16_to_float(tmp[0], tmp[1], 1.0f / 256.0f);
    }

    if (s_ak09_dev) {
        uint8_t st1;
        if (ak_read(AK_ST1, &st1, 1) == ESP_OK && (st1 & 0x01)) {
            uint8_t mag[6];
            if (ak_read(AK_HXL, mag, 6) == ESP_OK) {
                s->mag_present = true;
                s->mag_x = raw16_to_float(mag[0], mag[1], 0.15f);
                s->mag_y = raw16_to_float(mag[2], mag[3], 0.15f);
                s->mag_z = raw16_to_float(mag[4], mag[5], 0.15f);
            }
        }
    }
    return ESP_OK;
}

esp_err_t wr_imu_get_gyro(float *gz_dps)
{
    if (!gz_dps) return ESP_ERR_INVALID_ARG;
    if (!g_wr_i2c_bus) { *gz_dps = 0.0f; return ESP_OK; }
    if (!s_qmi_dev) {
        if (imu_hw_init() != ESP_OK) return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[6];
    ESP_RETURN_ON_ERROR(qmi_read(QMI_GYRX_L, raw, 6), TAG, "gyro_read");
    *gz_dps = raw16_to_float(raw[4], raw[5], 512.0f / 32768.0f) - s_gyro_off_z;
    return ESP_OK;
}

esp_err_t wr_imu_calibrate(uint16_t samples, uint32_t interval_ms,
                            wr_imu_calib_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    if (!g_wr_i2c_bus) {
        out->valid       = true;
        out->accel_ref_z = 1.0f;
        return ESP_OK;
    }

    if (!s_qmi_dev) {
        if (imu_hw_init() != ESP_OK) return ESP_ERR_INVALID_STATE;
    }

    if (samples < 2)   samples = 2;
    if (samples > 500) samples = 500;

    double sum_gx = 0, sum_gy = 0, sum_gz = 0, sum_az = 0;
    uint16_t good = 0;

    /* Temporarily clear offsets so we measure raw values */
    float saved_x = s_gyro_off_x, saved_y = s_gyro_off_y, saved_z = s_gyro_off_z;
    s_gyro_off_x = 0; s_gyro_off_y = 0; s_gyro_off_z = 0;

    for (uint16_t i = 0; i < samples; i++) {
        uint8_t raw[12];
        if (qmi_read(QMI_ACCX_L, raw, 12) == ESP_OK) {
            sum_gz += raw16_to_float(raw[10], raw[11], 512.0f / 32768.0f);
            sum_gy += raw16_to_float(raw[8],  raw[9],  512.0f / 32768.0f);
            sum_gx += raw16_to_float(raw[6],  raw[7],  512.0f / 32768.0f);
            sum_az += raw16_to_float(raw[4],  raw[5],  4.0f   / 32768.0f);
            good++;
        }
        if (interval_ms > 0) vTaskDelay(pdMS_TO_TICKS(interval_ms));
    }

    if (good < 2) {
        /* Restore old offsets on failure */
        s_gyro_off_x = saved_x;
        s_gyro_off_y = saved_y;
        s_gyro_off_z = saved_z;
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_gyro_off_x = (float)(sum_gx / good);
    s_gyro_off_y = (float)(sum_gy / good);
    s_gyro_off_z = (float)(sum_gz / good);

    out->valid         = true;
    out->gyro_offset_x = s_gyro_off_x;
    out->gyro_offset_y = s_gyro_off_y;
    out->gyro_offset_z = s_gyro_off_z;
    out->accel_ref_z   = (float)(sum_az / good);

    ESP_LOGI(TAG, "IMU calibrated: gyro_off=[%.4f %.4f %.4f] dps, accel_z=%.4f g",
             s_gyro_off_x, s_gyro_off_y, s_gyro_off_z, out->accel_ref_z);
    return ESP_OK;
}

esp_err_t wr_imu_set_mag_continuous(bool enable)
{
    if (!s_ak09_dev) return ESP_OK;
    /* AK09918: mode 1 = 10 Hz continuous, 0x00 = power-down */
    return ak_write(AK_CNTL2, enable ? AK_CNTL2_MODE_10HZ : AK_CNTL2_POWER_DOWN);
}
