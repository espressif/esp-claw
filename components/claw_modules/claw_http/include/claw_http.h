/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "claw_endpoint.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*claw_http_receive_fn)(const char *data, size_t size, void *ctx);

/** Send one request using an async ESP HTTP client owned by the caller.
 * The client must have is_async=true and its method/headers already configured.
 * Every I/O stage shares the caller's deadline and cancellation. Received body
 * bytes are borrowed only during receive(). Follows at most ten redirects
 * within the same budget. Callers inspect status and own credential retries.
 * The caller cleans up the client on every outcome.
 */
esp_err_t claw_http_send(esp_http_client_handle_t client, const char *body,
                         const claw_endpoint_request_t *request,
                         claw_http_receive_fn receive, void *ctx);

#ifdef __cplusplus
}
#endif
