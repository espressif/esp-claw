/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct claw_capability_registry claw_capability_registry_t;

int luaopen_capability(lua_State *L);

/* Register the `capability` Lua module. `registry` is the claw-cabi capability
 * registry that capability.call(name, ...) dispatches against; it must outlive
 * every Lua VM that imports this module. */
esp_err_t lua_module_capability_register(claw_capability_registry_t *registry);

#ifdef __cplusplus
}
#endif
