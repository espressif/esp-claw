/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SSE transport context (opaque)
 */
typedef struct cap_mcp_sse_ctx cap_mcp_sse_ctx_t;

/**
 * @brief Connect to an MCP SSE endpoint and extract the message POST URL.
 *
 * Performs HTTP GET to the SSE endpoint, reads the "endpoint" event,
 * and returns the session-specific POST URL for JSON-RPC messages.
 *
 * @param[in]  sse_url        Full SSE endpoint URL (e.g. http://host:3000/sse/device-id)
 * @param[in]  auth_token     Optional Bearer token (NULL if not needed)
 * @param[out] ctx_out        SSE context for subsequent requests
 * @param[out] post_url_out   Buffer to receive the POST URL from endpoint event
 * @param[in]  post_url_size  Size of post_url_out buffer
 * @return ESP_OK on success
 */
esp_err_t cap_mcp_sse_connect(const char *sse_url,
                              const char *auth_token,
                              cap_mcp_sse_ctx_t **ctx_out,
                              char *post_url_out,
                              size_t post_url_size);

/**
 * @brief Send a JSON-RPC request and wait for the response via SSE.
 *
 * POSTs the JSON-RPC body to the message endpoint, then reads the
 * SSE stream until a "message" event is received.
 *
 * @param[in]  ctx            SSE context from cap_mcp_sse_connect()
 * @param[in]  post_url       POST URL (from endpoint event)
 * @param[in]  auth_token     Optional Bearer token
 * @param[in]  json_rpc_body  JSON-RPC request body
 * @param[out] response       Buffer for JSON-RPC response
 * @param[in]  response_size  Size of response buffer
 * @return ESP_OK on success
 */
esp_err_t cap_mcp_sse_request(cap_mcp_sse_ctx_t *ctx,
                              const char *post_url,
                              const char *auth_token,
                              const char *json_rpc_body,
                              char *response,
                              size_t response_size);

/**
 * @brief Disconnect SSE and free resources.
 *
 * @param[in] ctx  SSE context to clean up
 */
void cap_mcp_sse_disconnect(cap_mcp_sse_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
