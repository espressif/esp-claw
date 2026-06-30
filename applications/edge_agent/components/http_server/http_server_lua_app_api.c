/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include "lua_module_http_server.h"

static esp_err_t lua_static_handler(httpd_req_t *req)
{
    return lua_module_http_server_handle_static(req);
}

static esp_err_t lua_api_handler(httpd_req_t *req)
{
    return lua_module_http_server_handle_api(req);
}

esp_err_t http_server_register_lua_app_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/lua/*", .method = HTTP_GET, .handler = lua_static_handler },
        { .uri = "/api/lua/*", .method = HTTP_GET, .handler = lua_api_handler },
        { .uri = "/api/lua/*", .method = HTTP_POST, .handler = lua_api_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
