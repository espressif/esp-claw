/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Per-chip backend interface for the lua_module_magnetometer module.
 *
 * Each supported chip (BMM350 / BMM150 / QMC6309 / ...) lives in its own
 * subfolder under `src/` and exports a single `lua_mag_backend_t` instance
 * named `lua_mag_backend`. The main glue file is chip-agnostic and only talks
 * to the backend through this table.
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
 * The main module owns this struct; backends only read/write the fields they
 * need (mainly `i2c_dev_handle` and the opaque `state` pointer).
 */
typedef struct {
    i2c_bus_handle_t i2c_bus_handle;       /*!< I2C bus that owns the device */
    i2c_bus_device_handle_t i2c_dev_handle; /*!< Active device handle (matches i2c_addr) */
    uint8_t i2c_addr;                      /*!< Active 7-bit I2C address */
    uint8_t chip_id;                       /*!< Filled by backend on successful probe */
    void *state;                           /*!< Backend-private state, sized by state_size */
} lua_mag_backend_ctx_t;

/**
 * @brief A single magnetometer reading.
 */
typedef struct {
    float x;
    float y;
    float z;
    float temperature; /*!< 0 when the chip has no temperature output */
    uint8_t status;
} lua_mag_sample_t;

/**
 * @brief Backend dispatch table. One instance per chip selection.
 */
typedef struct {
    const char *chip_name;
    size_t state_size; /*!< Bytes to allocate for `lua_mag_backend_ctx_t::state` */

    /** Probe the chip at `i2c_addr` and apply default runtime config. */
    esp_err_t (*probe)(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr);

    /** Read one magnetometer sample. */
    esp_err_t (*read_sample)(lua_mag_backend_ctx_t *ctx, lua_mag_sample_t *out);

    /** Read the chip-defined interrupt/status register. */
    esp_err_t (*read_status)(lua_mag_backend_ctx_t *ctx, uint8_t *out);

    /** Return whether `i2c_addr` is one of the chip's allowed I2C addresses. */
    bool (*is_supported_addr)(uint8_t i2c_addr);

    /** Default I2C address to try when neither board nor user override it. */
    uint8_t (*default_addr)(void);

    /** Try alternate I2C addresses after `primary` failed. Optional (may be NULL). */
    esp_err_t (*probe_alternates)(lua_mag_backend_ctx_t *ctx, uint8_t primary);
} lua_mag_backend_t;

/**
 * @brief Selected backend, defined in exactly one chip-specific .c file.
 */
extern const lua_mag_backend_t lua_mag_backend;

/* ---------------------------------------------------------------------------
 * Helpers exposed by the main module to backends.
 * ------------------------------------------------------------------------- */

/**
 * @brief Recreate the I2C device handle inside `ctx` for a new address.
 *
 * Backends use this when retrying alternate addresses during probe.
 */
esp_err_t lua_mag_ctx_select_addr(lua_mag_backend_ctx_t *ctx, uint8_t i2c_addr);

/**
 * @brief Portable microsecond delay (busy-wait for <1ms, vTaskDelay otherwise).
 *
 * Exposed for vendor SDK callbacks that need a delay function pointer.
 */
void lua_mag_delay_us(uint32_t period_us);

#ifdef __cplusplus
}
#endif
