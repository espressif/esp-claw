/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_http.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define HTTP_POLL_MS 100U
#define HTTP_MAX_REDIRECTS 10U

static bool is_redirect(int status)
{
    return status == HttpStatus_MovedPermanently || status == HttpStatus_Found ||
           status == HttpStatus_SeeOther || status == HttpStatus_TemporaryRedirect ||
           status == HttpStatus_PermanentRedirect;
}

static esp_err_t prepare_io(esp_http_client_handle_t client,
                             const claw_endpoint_request_t *request, bool connecting)
{
    esp_err_t err = claw_endpoint_request_check(request);
    if (err != ESP_OK) return err;
    uint32_t remaining = claw_endpoint_request_remaining_ms(request);
    if (!remaining) return ESP_ERR_TIMEOUT;
    uint32_t limit = connecting ? INT_MAX : HTTP_POLL_MS;
    return esp_http_client_set_timeout_ms(client, remaining < limit ? remaining : limit);
}

static esp_err_t send_once(esp_http_client_handle_t client, const char *body,
                         const claw_endpoint_request_t *request,
                         claw_http_receive_fn receive, void *ctx)
{
    if (!client || !body || !request || !receive) return ESP_ERR_INVALID_ARG;
    size_t size = strlen(body);
    if (size > INT_MAX) return ESP_ERR_INVALID_SIZE;
    esp_err_t err;
    do {
        err = prepare_io(client, request, true);
        if (err != ESP_OK) return err;
        err = esp_http_client_open(client, (int)size);
        if (err == ESP_ERR_HTTP_EAGAIN) vTaskDelay(1);
    } while (err == ESP_ERR_HTTP_EAGAIN);
    if (err != ESP_OK) return err;

    size_t sent = 0;
    while (sent < size) {
        err = prepare_io(client, request, false);
        if (err != ESP_OK) return err;
        errno = 0;
        int written = esp_http_client_write(client, body + sent, (int)(size - sent));
        if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return ESP_FAIL;
        if (written > 0) sent += written;
        else vTaskDelay(1);
    }

    for (;;) {
        err = prepare_io(client, request, false);
        if (err != ESP_OK) return err;
        int64_t length = esp_http_client_fetch_headers(client);
        if (length >= 0) break;
        if (length != -ESP_ERR_HTTP_EAGAIN) return ESP_ERR_INVALID_RESPONSE;
        vTaskDelay(1);
    }

    const bool redirect = is_redirect(esp_http_client_get_status_code(client));
    char buffer[128];
    for (;;) {
        err = prepare_io(client, request, false);
        if (err != ESP_OK) return err;
        int received = esp_http_client_read(client, buffer, sizeof(buffer));
        if (received == -ESP_ERR_HTTP_EAGAIN) {
            vTaskDelay(1);
            continue;
        }
        if (received < 0) return ESP_FAIL;
        if (received == 0) {
            if (!esp_http_client_is_complete_data_received(client)) return ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if (!redirect) {
            err = receive(buffer, received, ctx);
            if (err != ESP_OK) return err;
        }
    }
    return claw_endpoint_request_check(request);
}

esp_err_t claw_http_send(esp_http_client_handle_t client, const char *body,
                         const claw_endpoint_request_t *request,
                         claw_http_receive_fn receive, void *ctx)
{
    for (unsigned redirects = 0; ; redirects++) {
        esp_err_t err = send_once(client, body, request, receive, ctx);
        if (err != ESP_OK || !is_redirect(esp_http_client_get_status_code(client))) return err;
        if (redirects == HTTP_MAX_REDIRECTS) return ESP_ERR_HTTP_MAX_REDIRECT;
        err = esp_http_client_set_redirection(client);
        if (err != ESP_OK) return err;
        err = esp_http_client_close(client);
        if (err != ESP_OK) return err;
    }
}
