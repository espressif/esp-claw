/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_http_server(lua_State *L);
esp_err_t lua_module_http_server_register(void);

esp_err_t lua_module_http_server_handle_static(httpd_req_t *req);
esp_err_t lua_module_http_server_handle_api(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
