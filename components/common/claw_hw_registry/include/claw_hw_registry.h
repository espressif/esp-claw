/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Explicit resource registration and atomic usage claims. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAW_HW_KEY_SIZE 48
#define CLAW_HW_OWNER_SIZE 48
#define CLAW_HW_CLAIM_INVALID UINT64_C(0)
#define CLAW_HW_ERR_CONFLICT ((esp_err_t)0x7c01)

typedef struct claw_hw_registry_t *claw_hw_registry_handle_t;
typedef uint64_t claw_hw_claim_id_t;
typedef enum { CLAW_HW_EXCLUSIVE = 0, CLAW_HW_SHARED } claw_hw_policy_t;

typedef struct {
    const char *resource;
    claw_hw_policy_t policy;
} claw_hw_resource_config_t;

typedef struct {
    const char *owner;
    const char *const *resources;
    size_t resource_count;
} claw_hw_claim_config_t;

typedef struct {
    claw_hw_claim_id_t claim_id;
    char resource[CLAW_HW_KEY_SIZE];
    char owner[CLAW_HW_OWNER_SIZE];
    claw_hw_policy_t policy;
    size_t users;
} claw_hw_entry_t;

/* Delete requires an empty registry and no concurrent callers. */
esp_err_t claw_hw_registry_create(claw_hw_registry_handle_t *ret_handle);
esp_err_t claw_hw_registry_delete(claw_hw_registry_handle_t handle);
/* All batches are atomic. Register/claim may return ESP_ERR_NO_MEM when growing tables. */
esp_err_t claw_hw_registry_register(claw_hw_registry_handle_t handle, const claw_hw_resource_config_t *resources, size_t count);
esp_err_t claw_hw_registry_unregister(claw_hw_registry_handle_t handle, const char *const *resources, size_t count);
/* Canonical i2c_addr:<port>:<hex> keys require a registered i2c_bus:<port> and are always exclusive. */
esp_err_t claw_hw_registry_claim(claw_hw_registry_handle_t handle, const claw_hw_claim_config_t *config, claw_hw_claim_id_t *ret_id);
/* Stop hardware access before release. Dynamic address entries disappear on release. */
esp_err_t claw_hw_registry_release(claw_hw_registry_handle_t handle, claw_hw_claim_id_t id);
/* NULL resource selects all. Insufficient capacity returns the required count without writing entries. */
esp_err_t claw_hw_registry_query(claw_hw_registry_handle_t handle, const char *resource, claw_hw_entry_t *entries, size_t capacity, size_t *count);

/* Always use these builders for keys; do not hard-code or format keys at call sites.
 * Builders return NULL and clear buf on invalid arguments or truncation. */
const char *claw_hw_key_gpio(char *buf, size_t size, int pin);
const char *claw_hw_key_uart_bus(char *buf, size_t size, int port);
const char *claw_hw_key_i2c_controller(char *buf, size_t size, int port);
const char *claw_hw_key_i2c_addr(char *buf, size_t size, int port, int address);
const char *claw_hw_key_spi_bus(char *buf, size_t size, int host);
const char *claw_hw_key_i2s_bus(char *buf, size_t size, int port);
const char *claw_hw_key_adc_unit(char *buf, size_t size, int unit);
const char *claw_hw_key_adc_channel(char *buf, size_t size, int unit, int channel);
const char *claw_hw_key_ledc_timer(char *buf, size_t size, int mode, int timer);
const char *claw_hw_key_ledc_channel(char *buf, size_t size, int mode, int channel);
const char *claw_hw_key_sdmmc_slot(char *buf, size_t size, int slot);
const char *claw_hw_key_dsi_bus(char *buf, size_t size, int bus);
const char *claw_hw_key_ldo_channel(char *buf, size_t size, int channel);
const char *claw_hw_key_device(char *buf, size_t size, const char *name);

#ifdef __cplusplus
}
#endif
