/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

typedef struct lua_State lua_State;

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_ble_hid(lua_State *L);
esp_err_t lua_module_ble_hid_register(void);

#ifdef __cplusplus
}
#endif
