/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "lua.h"

#define CAP_LUA_JOB_ID_LEN 9

esp_err_t cap_lua_register_module(const char *name, lua_CFunction openf);
esp_err_t cap_lua_register_exit_cleanup(void (*cleanup)(lua_State *L));
bool cap_lua_runtime_stop_requested(lua_State *L);
const char *cap_lua_runtime_job_id(lua_State *L);
esp_err_t cap_lua_stop_job(const char *id_or_name, uint32_t wait_ms, char *output, size_t output_size);
