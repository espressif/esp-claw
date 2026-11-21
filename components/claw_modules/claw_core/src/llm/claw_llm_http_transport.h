/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "claw_endpoint.h"
#include "llm/claw_llm_types.h"

esp_err_t claw_llm_http_post_json(const claw_llm_http_json_request_t *request,
                                  claw_llm_http_response_t *out_response,
                                  char **out_error_message);

/**
 * @brief POST to `path` under the endpoint URL, adding the endpoint headers.
 *
 * `request->url` must be NULL. The outcome is reported to the endpoint; after
 * HTTP 401 the request is retried once only when the endpoint asks for it.
 * Returns CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED when credentials stay rejected.
 */
esp_err_t claw_llm_http_post_endpoint(claw_endpoint_handle_t endpoint,
                                      const char *path,
                                      const claw_llm_http_json_request_t *request,
                                      claw_llm_http_response_t *out_response,
                                      char **out_error_message);
void claw_llm_http_response_free(claw_llm_http_response_t *response);
