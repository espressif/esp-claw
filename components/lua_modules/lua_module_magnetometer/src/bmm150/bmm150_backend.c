/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include "bmm150.h"
#include "bmm150_common.h"
#include "lua_module_mag_backend.h"

static esp_err_t bmm150_create(lua_mag_backend_ctx_t *ctx, uint8_t address)
{
    struct bmm150_dev *sensor = calloc(1, sizeof(*sensor));
    if (!sensor) return ESP_ERR_NO_MEM;
    ctx->sensor = sensor;
    esp_err_t err = bmm150_interface_init_from_master_bus(sensor, ctx->master_bus, address, ctx->frequency);
    if (err != ESP_OK) return err;
    if (bmm150_init(sensor) != BMM150_OK || sensor->chip_id != BMM150_CHIP_ID) return ESP_FAIL;
    struct bmm150_settings settings = {.pwr_mode = BMM150_POWERMODE_NORMAL, .preset_mode = BMM150_PRESETMODE_REGULAR};
    if (bmm150_set_presetmode(&settings, sensor) != BMM150_OK || bmm150_set_op_mode(&settings, sensor) != BMM150_OK) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t bmm150_destroy(lua_mag_backend_ctx_t *ctx)
{
    struct bmm150_dev *sensor = ctx->sensor;
    if (!sensor) return ESP_OK;
    if (sensor->intf_ptr) {
        esp_err_t err = bmm150_interface_deinit_device(sensor);
        if (err != ESP_OK) return err;
    }
    free(sensor);
    ctx->sensor = NULL;
    return ESP_OK;
}

static esp_err_t bmm150_read_sample(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out)
{
    struct bmm150_mag_data data = {0};
    if (bmm150_read_mag_data(&data, ctx->sensor) != BMM150_OK) return ESP_FAIL;
    out->x = data.x;
    out->y = data.y;
    out->z = data.z;
    return bmm150_get_regs(BMM150_REG_INTERRUPT_STATUS, &out->status, 1, ctx->sensor) == BMM150_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t bmm150_read_status(lua_mag_backend_ctx_t *ctx, uint8_t *out)
{
    return bmm150_get_regs(BMM150_REG_INTERRUPT_STATUS, out, 1, ctx->sensor) == BMM150_OK ? ESP_OK : ESP_FAIL;
}

static bool bmm150_is_supported_addr(uint8_t address)
{
    return address >= BMM150_DEFAULT_I2C_ADDRESS && address <= BMM150_I2C_ADDRESS_CSB_HIGH_SDO_HIGH;
}

static uint8_t bmm150_default_addr(void)
{
    return BMM150_DEFAULT_I2C_ADDRESS;
}

static int bmm150_sdo_level(uint8_t address)
{
    return address & 1;
}

const lua_mag_backend_t lua_mag_backend_bmm150 = {
    .chip_id = BMM150_CHIP_ID,
    .create = bmm150_create, .destroy = bmm150_destroy,
    .read_sample = bmm150_read_sample, .read_status = bmm150_read_status,
    .is_supported_addr = bmm150_is_supported_addr, .default_addr = bmm150_default_addr,
    .sdo_level_for_addr = bmm150_sdo_level,
};
