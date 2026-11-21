/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_hw_registry.h"
#include "lua.h"

typedef struct {
    claw_hw_registry_handle_t registry;
    claw_hw_claim_id_t id;
} cap_lua_hw_lease_t;

void cap_lua_hw_bind(lua_State *L, claw_hw_registry_handle_t registry);
claw_hw_registry_handle_t cap_lua_hw_get_registry(lua_State *L);
esp_err_t cap_lua_hw_claim(lua_State *L, cap_lua_hw_lease_t *lease, const char *owner, const char *const *keys, size_t count);
esp_err_t cap_lua_hw_release(cap_lua_hw_lease_t *lease);
esp_err_t cap_lua_hw_claim_gpio(lua_State *L, cap_lua_hw_lease_t *lease, const char *owner, int gpio);

/* Short SDK operations from ordinary tasks only; no nesting, ESP_TIMER_TASK, or ISR calls. */
esp_err_t cap_lua_hw_timer_call(esp_err_t (*operation)(void *), void *arg);
