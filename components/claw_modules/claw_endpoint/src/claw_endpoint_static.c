/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_endpoint_static.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "claw_utils_string.h"

#define BEARER_PREFIX "Bearer "
#define HEADER_AUTHORIZATION "Authorization"
#define HEADER_API_KEY "X-API-Key"

typedef struct {
    char *url;
    const char *auth_header_name;
    char *auth_header_value;
} static_endpoint_t;

static bool string_is_empty(const char *value)
{
    return !value || !value[0];
}

static bool static_is_configured(const claw_endpoint_config_t *config)
{
    if (string_is_empty(config->base_url)) return false;
    const char *auth = string_is_empty(config->auth_type) ? CLAW_ENDPOINT_AUTH_BEARER : config->auth_type;
    if (strcmp(auth, CLAW_ENDPOINT_AUTH_NONE) == 0) return true;
    return (strcmp(auth, CLAW_ENDPOINT_AUTH_BEARER) == 0 ||
            strcmp(auth, CLAW_ENDPOINT_AUTH_API_KEY) == 0) && !string_is_empty(config->api_key);
}

static esp_err_t build_auth_header(const claw_endpoint_config_t *config, static_endpoint_t *endpoint)
{
    const char *auth_type = string_is_empty(config->auth_type) ? CLAW_ENDPOINT_AUTH_BEARER : config->auth_type;
    const char *api_key = config->api_key;

    if (strcmp(auth_type, CLAW_ENDPOINT_AUTH_NONE) == 0) {
        return ESP_OK;
    }
    if (strcmp(auth_type, CLAW_ENDPOINT_AUTH_API_KEY) == 0) {
        endpoint->auth_header_name = HEADER_API_KEY;
        endpoint->auth_header_value = claw_utils_string_dup(api_key);
    } else {
        endpoint->auth_header_name = HEADER_AUTHORIZATION;
        endpoint->auth_header_value =
            strncasecmp(api_key, BEARER_PREFIX, strlen(BEARER_PREFIX)) == 0
                ? claw_utils_string_dup(api_key)
                : claw_utils_string_dup_printf(BEARER_PREFIX "%s", api_key);
    }
    return endpoint->auth_header_value ? ESP_OK : ESP_ERR_NO_MEM;
}

static void static_destroy(void *ctx)
{
    static_endpoint_t *endpoint = ctx;

    if (!endpoint) {
        return;
    }
    free(endpoint->url);
    free(endpoint->auth_header_value);
    free(endpoint);
}

static esp_err_t static_create(const claw_endpoint_config_t *config, void **out_ctx)
{
    if (!static_is_configured(config)) {
        return ESP_ERR_INVALID_ARG;
    }

    static_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) {
        return ESP_ERR_NO_MEM;
    }
    endpoint->url = claw_utils_string_dup(config->base_url);
    esp_err_t err = endpoint->url ? build_auth_header(config, endpoint) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        static_destroy(endpoint);
        return err;
    }
    *out_ctx = endpoint;
    return ESP_OK;
}

static esp_err_t static_acquire(void *ctx, const claw_endpoint_request_t *request,
                                claw_endpoint_resolved_t *out)
{
    static_endpoint_t *endpoint = ctx;
    (void)request;

    esp_err_t err = claw_endpoint_resolved_set_url(out, endpoint->url);
    if (err == ESP_OK && endpoint->auth_header_value) {
        err = claw_endpoint_resolved_add_header(out, endpoint->auth_header_name,
                                                endpoint->auth_header_value);
    }
    return err;
}

static const claw_endpoint_provider_ops_t s_static_provider = {
    .is_configured = static_is_configured,
    .create = static_create,
    .acquire = static_acquire,
    .destroy = static_destroy,
};

const claw_endpoint_provider_ops_t *claw_endpoint_static_provider(void)
{
    return &s_static_provider;
}
