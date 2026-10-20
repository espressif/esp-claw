/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* Board handles: bmm150_dev*, bmm350_dev*, or a QMC6309 I2C device handle. */
typedef struct {
    i2c_master_bus_handle_t master_bus;
    i2c_master_dev_handle_t i2c_dev_handle;
    uint32_t frequency;
    void *sensor;
} lua_mag_backend_ctx_t;

typedef struct {
    float x, y, z;
    float temperature;
    uint8_t status;
} lua_mag_sample_t;

typedef struct {
    uint8_t chip_id;
    bool has_temperature;
    esp_err_t (*create)(lua_mag_backend_ctx_t *ctx, uint8_t address);
    esp_err_t (*destroy)(lua_mag_backend_ctx_t *ctx);
    esp_err_t (*read_sample)(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out);
    esp_err_t (*read_status)(lua_mag_backend_ctx_t *ctx, uint8_t *out);
    bool (*is_supported_addr)(uint8_t address);
    uint8_t (*default_addr)(void);
    int (*sdo_level_for_addr)(uint8_t address);
} lua_mag_backend_t;

extern const lua_mag_backend_t lua_mag_backend_bmm150;
extern const lua_mag_backend_t lua_mag_backend_bmm350;
extern const lua_mag_backend_t lua_mag_backend_qmc6309;
