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

int luaopen_mqtt(lua_State *L);
esp_err_t lua_module_mqtt_register(void);

/**
 * @brief Set default broker connection values used by `mqtt.new()`.
 *
 * Scripts may call `mqtt.new()` with no uri (or omit credential fields) to
 * reuse these defaults; explicit arguments always override them. The strings
 * are copied internally; callers may free their buffers after the call. Pass
 * NULL or "" for any field that has no default. Intended to be called once at
 * boot by the application layer, which owns the persistent broker settings.
 */
esp_err_t lua_module_mqtt_set_defaults(const char *uri,
                                       const char *username,
                                       const char *password,
                                       const char *client_id);

#ifdef __cplusplus
}
#endif
