/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the ir.protocol namespace (list, get, register, none) in the module table at the top of the stack. */
void lua_ir_protocol_register(lua_State *L);

#ifdef __cplusplus
}
#endif
