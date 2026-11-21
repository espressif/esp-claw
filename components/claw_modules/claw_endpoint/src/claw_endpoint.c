/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_endpoint.h"

#include <stdlib.h>
#include <string.h>

#include "claw_endpoint_static.h"
#include "claw_utils_string.h"
#include "esp_timer.h"

typedef struct provider_node {
    const char *id;
    const claw_endpoint_provider_ops_t *ops;
    struct provider_node *next;
} provider_node_t;

struct claw_endpoint_t {
    const claw_endpoint_provider_ops_t *ops;
    void *ctx;
};

static provider_node_t *s_providers;

void claw_endpoint_request_init(claw_endpoint_request_t *request, uint32_t timeout_ms,
                                 bool (*is_cancelled)(void *ctx), void *cancel_ctx)
{
    if (request) {
        *request = (claw_endpoint_request_t) {
            .deadline_ms = esp_timer_get_time() / 1000 + timeout_ms,
            .is_cancelled = is_cancelled,
            .cancel_ctx = cancel_ctx,
        };
    }
}

uint32_t claw_endpoint_request_remaining_ms(const claw_endpoint_request_t *request)
{
    if (!request) return 0;
    int64_t remaining = request->deadline_ms - esp_timer_get_time() / 1000;
    return remaining <= 0 ? 0 : (remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining);
}

esp_err_t claw_endpoint_request_check(const claw_endpoint_request_t *request)
{
    if (!request) return ESP_ERR_INVALID_ARG;
    if (request->is_cancelled && request->is_cancelled(request->cancel_ctx)) {
        return CLAW_ENDPOINT_ERR_CANCELLED;
    }
    return claw_endpoint_request_remaining_ms(request) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static bool ops_are_complete(const claw_endpoint_provider_ops_t *ops)
{
    return ops && ops->is_configured && ops->create && ops->acquire && ops->destroy;
}

static const claw_endpoint_provider_ops_t *find_provider(const char *id)
{
    if (!id || !id[0] || strcmp(id, CLAW_ENDPOINT_PROVIDER_STATIC) == 0) {
        return claw_endpoint_static_provider();
    }
    for (provider_node_t *node = s_providers; node; node = node->next) {
        if (strcmp(node->id, id) == 0) {
            return node->ops;
        }
    }
    return NULL;
}

esp_err_t claw_endpoint_register_provider(const char *id, const claw_endpoint_provider_ops_t *ops)
{
    if (!id || !id[0] || !ops_are_complete(ops)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_provider(id)) {
        return ESP_ERR_INVALID_STATE;
    }

    provider_node_t *node = calloc(1, sizeof(*node));
    if (!node) {
        return ESP_ERR_NO_MEM;
    }
    node->id = id;
    node->ops = ops;
    node->next = s_providers;
    s_providers = node;
    return ESP_OK;
}

bool claw_endpoint_is_configured(const char *provider_id, const claw_endpoint_config_t *config)
{
    const claw_endpoint_provider_ops_t *ops = find_provider(provider_id);

    return ops && config && ops->is_configured(config);
}

esp_err_t claw_endpoint_create(const char *provider_id,
                               const claw_endpoint_config_t *config,
                               claw_endpoint_handle_t *ret_handle)
{
    if (!config || !ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handle = NULL;

    const claw_endpoint_provider_ops_t *ops = find_provider(provider_id);
    if (!ops) {
        return ESP_ERR_NOT_FOUND;
    }

    claw_endpoint_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }
    handle->ops = ops;
    esp_err_t err = ops->create(config, &handle->ctx);
    if (err != ESP_OK) {
        free(handle);
        return err;
    }
    *ret_handle = handle;
    return ESP_OK;
}

esp_err_t claw_endpoint_acquire(claw_endpoint_handle_t handle,
                                const claw_endpoint_request_t *request,
                                claw_endpoint_resolved_t *out)
{
    if (!handle || !request || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    esp_err_t err = claw_endpoint_request_check(request);
    if (err == ESP_OK) {
        err = handle->ops->acquire(handle->ctx, request, out);
    }
    if (err == ESP_OK) {
        err = claw_endpoint_request_check(request);
    }
    if (err == ESP_OK && (!out->url || !out->url[0])) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err != ESP_OK) {
        claw_endpoint_resolved_free(out);
    }
    return err;
}

bool claw_endpoint_report(claw_endpoint_handle_t handle,
                          const claw_endpoint_resolved_t *resolved,
                          claw_endpoint_outcome_t outcome)
{
    if (!handle || !resolved || !handle->ops->report) {
        return false;
    }
    bool retry = handle->ops->report(handle->ctx, resolved, outcome);
    return outcome == CLAW_ENDPOINT_OUTCOME_REJECTED && retry;
}

const char *claw_endpoint_err_to_name(esp_err_t err)
{
    switch (err) {
    case CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE:
        return "CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE";
    case CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED:
        return "CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED";
    case CLAW_ENDPOINT_ERR_CANCELLED:
        return "CLAW_ENDPOINT_ERR_CANCELLED";
    default:
        return esp_err_to_name(err);
    }
}

void claw_endpoint_delete(claw_endpoint_handle_t handle)
{
    if (!handle) {
        return;
    }
    handle->ops->destroy(handle->ctx);
    free(handle);
}

esp_err_t claw_endpoint_resolved_set_url(claw_endpoint_resolved_t *resolved, const char *url)
{
    if (!resolved || !url || !url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    char *copy = claw_utils_string_dup(url);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    free(resolved->url);
    resolved->url = copy;
    return ESP_OK;
}

esp_err_t claw_endpoint_resolved_add_header(claw_endpoint_resolved_t *resolved,
                                            const char *name,
                                            const char *value)
{
    if (!resolved || !name || !name[0] || !value) {
        return ESP_ERR_INVALID_ARG;
    }
    if (resolved->header_count >= CLAW_ENDPOINT_MAX_HEADERS) {
        return ESP_ERR_INVALID_SIZE;
    }

    claw_endpoint_header_t *header = &resolved->headers[resolved->header_count];
    header->name = claw_utils_string_dup(name);
    header->value = claw_utils_string_dup(value);
    if (!header->name || !header->value) {
        free(header->name);
        free(header->value);
        memset(header, 0, sizeof(*header));
        return ESP_ERR_NO_MEM;
    }
    resolved->header_count++;
    return ESP_OK;
}

void claw_endpoint_resolved_free(claw_endpoint_resolved_t *resolved)
{
    if (!resolved) {
        return;
    }
    free(resolved->url);
    for (size_t i = 0; i < resolved->header_count; i++) {
        free(resolved->headers[i].name);
        free(resolved->headers[i].value);
    }
    memset(resolved, 0, sizeof(*resolved));
}
