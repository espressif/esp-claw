/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mirror esp_board_manager state into claw_hw_registry so raw Lua drivers
 * cannot collide with pins/addresses owned by board-manager devices.
 */
#pragma once

#include "claw_hw_registry.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Iterate every initialized board device once and register its underlying
 * GPIO/I2C/SPI/I2S resources as EXCLUSIVE under "board/<device_name>".
 * dev:<name> keys are registered lazily by app_claw_hw_lease_device.
 * Idempotent. */
esp_err_t app_claw_hw_bridge_register_board_devices(void);

/* Lazily register dev:<device_name> under owner_tag/mode (a second call
 * with a compatible SHARED_READ coexists; otherwise ESP_ERR_INVALID_STATE)
 * and hand back the esp_board_manager device handle. out_lease is required;
 * out_device_handle may be NULL. */
esp_err_t app_claw_hw_lease_device(const char *device_name,
                                   const char *owner_tag,
                                   claw_hw_mode_t mode,
                                   claw_hw_lease_handle_t *out_lease,
                                   void **out_device_handle);

#ifdef __cplusplus
}
#endif
