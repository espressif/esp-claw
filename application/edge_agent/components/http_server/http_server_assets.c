/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

static esp_err_t favicon_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req,
                                          favicon_ico_start,
                                          favicon_ico_end,
                                          "image/x-icon");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    return http_server_send_embedded_file(req,
                                          index_html_gz_start,
                                          index_html_gz_end,
                                          "text/html; charset=utf-8");
}

esp_err_t http_server_register_assets_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/",           .method = HTTP_GET, .handler = index_handler },
        { .uri = "/index.html", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
