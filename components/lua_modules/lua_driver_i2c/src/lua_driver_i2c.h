/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "lua.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_i2c(lua_State *L);
esp_err_t lua_driver_i2c_register(void);

typedef struct lua_driver_i2c_bus *lua_driver_i2c_bus_handle_t;
/* The caller must retain the Lua bus userdata until unref. */
i2c_master_bus_handle_t lua_driver_i2c_ref_bus(lua_State *L, int index, int *port, lua_driver_i2c_bus_handle_t *owner);
void lua_driver_i2c_unref_bus(lua_driver_i2c_bus_handle_t owner);

#ifdef __cplusplus
}
#endif
