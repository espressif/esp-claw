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
#include "lua_module_mag_backend.h"

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

static esp_err_t bmm350_create(lua_mag_backend_ctx_t *ctx, uint8_t address)
{
    struct bmm350_dev *sensor = calloc(1, sizeof(*sensor));
    if (!sensor) return ESP_ERR_NO_MEM;
    ctx->sensor = sensor;
    i2c_device_config_t config = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = ctx->frequency};
    esp_err_t err = i2c_master_bus_add_device(ctx->master_bus, &config, &ctx->i2c_dev_handle);
    if (err != ESP_OK) return err;
    sensor->intf_ptr = ctx->i2c_dev_handle;
    sensor->read = sensor_read;
    sensor->write = sensor_write;
    sensor->delay_us = sensor_delay;
    if (bmm350_init(sensor) != BMM350_OK || sensor->chip_id != BMM350_CHIP_ID ||
        bmm350_set_odr_performance(BMM350_DATA_RATE_100HZ, BMM350_AVERAGING_4, sensor) != BMM350_OK ||
        bmm350_enable_axes(BMM350_X_EN, BMM350_Y_EN, BMM350_Z_EN, sensor) != BMM350_OK ||
        bmm350_set_powermode(BMM350_NORMAL_MODE, sensor) != BMM350_OK) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t bmm350_destroy(lua_mag_backend_ctx_t *ctx)
{
    if (ctx->i2c_dev_handle) {
        esp_err_t err = i2c_master_bus_rm_device(ctx->i2c_dev_handle);
        if (err != ESP_OK) return err;
        ctx->i2c_dev_handle = NULL;
    }
    free(ctx->sensor);
    ctx->sensor = NULL;
    return ESP_OK;
}

static esp_err_t bmm350_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    struct bmm350_mag_temp_data data = {0};
    if (bmm350_get_compensated_mag_xyz_temp_data(&data, ctx->sensor) != BMM350_OK) return ESP_FAIL;
    out->x = data.x;
    out->y = data.y;
    out->z = data.z;
    out->temperature = data.temperature;
    return bmm350_get_regs(BMM350_REG_INT_STATUS, &out->status, 1, ctx->sensor) == BMM350_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t bmm350_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    return bmm350_get_regs(BMM350_REG_INT_STATUS, out, 1, ctx->sensor) == BMM350_OK ? ESP_OK : ESP_FAIL;
}

static bool bmm350_is_supported_addr(uint8_t address)
{
    return address == BMM350_I2C_ADSEL_SET_LOW || address == BMM350_I2C_ADSEL_SET_HIGH;
}

static uint8_t bmm350_default_addr(void)
{
    return BMM350_I2C_ADSEL_SET_LOW;
}

static int bmm350_sdo_level(uint8_t address)
{
    return address == BMM350_I2C_ADSEL_SET_HIGH;
}

const lua_mag_backend_t lua_mag_backend_bmm350 = {
    .chip_id = BMM350_CHIP_ID, .has_temperature = true,
    .create = bmm350_create, .destroy = bmm350_destroy,
    .read_sample = bmm350_read_sample, .read_status = bmm350_read_status,
    .is_supported_addr = bmm350_is_supported_addr, .default_addr = bmm350_default_addr,
    .sdo_level_for_addr = bmm350_sdo_level,
};
