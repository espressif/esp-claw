/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-chip backend interface for the lua_module_imu module.
 *
 * Each supported IMU chip (BMI270 / ICM42670 / MPU6050 / ...) lives under
 * src/<chip>/ and exports a single `lua_imu_backend_t` instance named
 * `lua_imu_backend`. The main glue file talks to chips only via this table.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "i2c_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Shared per-instance context handed to the backend.
 *
 * The main module owns this struct. Backends only read/write what they need:
 *   - `i2c_bus_handle` (e.g. BMI270 takes the wrapper bus directly);
 *   - `i2c_dev_handle` (e.g. MPU6050 reads/writes by register through it);
 *   - `state` (backend-private scratch sized by `state_size`).
 */
typedef struct {
    i2c_bus_handle_t i2c_bus_handle;
    i2c_bus_device_handle_t i2c_dev_handle;
    uint8_t i2c_addr;
    void *state;
} lua_imu_backend_ctx_t;

typedef struct {
    int x;
    int y;
    int z;
} lua_imu_axes_t;

typedef struct {
    lua_imu_axes_t accel;
    lua_imu_axes_t gyro;
    int64_t sens_time;   /*!< microseconds; backend may fill from sensor or wall-clock */
    uint32_t status;     /*!< chip-specific status bits */
} lua_imu_sample_t;

typedef struct {
    const char *chip_name;
    size_t state_size; /*!< Bytes for `ctx->state`. 0 if backend keeps no private state. */

    /** Probe + initialize the chip at `i2c_addr`. */
    esp_err_t (*probe)(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr);

    /** Tear down backend-private resources. May be NULL. */
    void (*destroy)(lua_imu_backend_ctx_t *ctx);

    /** Read one accel+gyro sample. */
    esp_err_t (*read_sample)(lua_imu_backend_ctx_t *ctx, lua_imu_sample_t *out);

    /** Read raw temperature (chip-defined units). */
    esp_err_t (*read_temperature)(lua_imu_backend_ctx_t *ctx, int32_t *out);

    /** Read combined interrupt status. */
    esp_err_t (*read_int_status)(lua_imu_backend_ctx_t *ctx, uint32_t *out);

    /** Return whether `i2c_addr` is one of the chip's allowed addresses. */
    bool (*is_supported_addr)(uint8_t i2c_addr);

    /** Default I2C address used when board/Lua does not override it. */
    uint8_t (*default_addr)(void);

    /**
     * Return the level (0 or 1) the SDO/AD0/ADSEL pin must be driven to
     * in order to select `i2c_addr`. Used only when a SDO GPIO is wired;
     * if no SDO GPIO is configured, the main module never touches any pin.
     */
    int (*sdo_level_for_addr)(uint8_t i2c_addr);
} lua_imu_backend_t;

extern const lua_imu_backend_t lua_imu_backend;

/* ---------------------------------------------------------------------------
 * Helpers exposed by the main module to backends.
 * ------------------------------------------------------------------------- */

/**
 * @brief (Re)create the per-device I2C handle inside `ctx` for `i2c_addr`.
 */
esp_err_t lua_imu_ctx_select_addr(lua_imu_backend_ctx_t *ctx, uint8_t i2c_addr);

#ifdef __cplusplus
}
#endif
