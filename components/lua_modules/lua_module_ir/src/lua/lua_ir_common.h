/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "ir_fields.h"
#include "ir_protocol.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reads field values from the table at index; an unknown key raises an error. */
void lua_ir_values_read(lua_State *L, int index, const char *context, ir_values_t *values);
/* Sets every present value as a field of the table at the top of the stack. */
void lua_ir_values_write(lua_State *L, const ir_values_t *values);
/* Pushes one value in its Lua form: boolean, enum name, number or integer. */
void lua_ir_value_push(lua_State *L, ir_field_id_t id, int32_t value);
/* Raises a Lua error unless every key of the table at index is listed in allowed. */
void lua_ir_check_keys(lua_State *L, int index, const char *context, const char *const *allowed, size_t allowed_count);
/* Reason returned as `nil, reason` for a failed decode, or NULL when the error is not recoverable. */
const char *lua_ir_reason(esp_err_t err);
/* Human readable text for an encode error. */
const char *lua_ir_error_text(esp_err_t err);

#ifdef __cplusplus
}
#endif
