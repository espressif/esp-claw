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

#ifdef __cplusplus
extern "C" {
#endif

/** Built-in provider: the configured base URL plus an API key header. */
#define CLAW_ENDPOINT_PROVIDER_STATIC "static"

/** Well-known logical service names passed in claw_endpoint_config_t::service. */
#define CLAW_ENDPOINT_SERVICE_LLM "llm"
#define CLAW_ENDPOINT_SERVICE_ASR "asr"

/** Auth styles understood by the static provider. */
#define CLAW_ENDPOINT_AUTH_BEARER  "bearer"
#define CLAW_ENDPOINT_AUTH_API_KEY "api-key"
#define CLAW_ENDPOINT_AUTH_NONE    "none"

#define CLAW_ENDPOINT_MAX_HEADERS 4

#define CLAW_ENDPOINT_ERR_BASE 0x1C000
/** The credential issuer refused to issue credentials. */
#define CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE (CLAW_ENDPOINT_ERR_BASE + 1)
/** The service rejected the credentials and acquiring again will not help. */
#define CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED (CLAW_ENDPOINT_ERR_BASE + 2)
#define CLAW_ENDPOINT_ERR_CANCELLED (CLAW_ENDPOINT_ERR_BASE + 3)

typedef struct claw_endpoint_t *claw_endpoint_handle_t;

typedef enum {
    CLAW_ENDPOINT_OUTCOME_ACCEPTED = 0,
    /** The service answered HTTP 401 for these credentials. */
    CLAW_ENDPOINT_OUTCOME_REJECTED,
} claw_endpoint_outcome_t;

/**
 * User-facing connection settings. Each provider decides how to interpret
 * them; the static provider uses them verbatim.
 */
typedef struct {
    const char *service;
    const char *base_url;
    const char *api_key;
    const char *auth_type;
} claw_endpoint_config_t;

/** One caller's budget, shared by discovery, credentials, transport and retry.
 * The callback and its context must remain valid until the operation returns.
 * Cancelling a waiter must not cancel another caller's shared refresh.
 */
typedef struct {
    int64_t deadline_ms; /**< Absolute monotonic time, in milliseconds. */
    bool (*is_cancelled)(void *ctx);
    void *cancel_ctx;
} claw_endpoint_request_t;

void claw_endpoint_request_init(claw_endpoint_request_t *request, uint32_t timeout_ms,
                                 bool (*is_cancelled)(void *ctx), void *cancel_ctx);
esp_err_t claw_endpoint_request_check(const claw_endpoint_request_t *request);
uint32_t claw_endpoint_request_remaining_ms(const claw_endpoint_request_t *request);

typedef struct {
    char *name;
    char *value;
} claw_endpoint_header_t;

/** Owned snapshot for one request. All strings must be independently allocated
 * with the provider helpers below (never borrowed). Free after report/transport
 * with claw_endpoint_resolved_free(). Do not reuse a live result as acquire output.
 */
typedef struct {
    char *url;
    claw_endpoint_header_t headers[CLAW_ENDPOINT_MAX_HEADERS];
    size_t header_count;
    /** Provider-defined id of the credentials; echoed back in report(). */
    uint32_t credential_id;
} claw_endpoint_resolved_t;

typedef struct {
    /** Cheap configuration check without network access. */
    bool (*is_configured)(const claw_endpoint_config_t *config);
    /** Copy retained configuration. On failure clean up internally and leave
     * out_ctx NULL; destroy is called only after successful creation. */
    esp_err_t (*create)(const claw_endpoint_config_t *config, void **out_ctx);
    /** Honour the deadline and cancellation, including lock and network waits.
     * The framework supplies zeroed output and frees partial output on failure.
     * acquire/report may run concurrently; providers synchronize mutable state.
     */
    esp_err_t (*acquire)(void *ctx, const claw_endpoint_request_t *request,
                         claw_endpoint_resolved_t *out);
    /**
     * Optional. Learn whether the service accepted the credentials of
     * `resolved`. For REJECTED, return true only when acquiring again is
     * expected to yield different credentials worth one retry.
     * Borrows resolved. Must not perform network I/O or wait for a refresh.
     */
    bool (*report)(void *ctx, const claw_endpoint_resolved_t *resolved,
                   claw_endpoint_outcome_t outcome);
    void (*destroy)(void *ctx);
} claw_endpoint_provider_ops_t;

/**
 * @brief Register an endpoint provider.
 *
 * Register during startup, before any endpoint is created. `id` and `ops`
 * must stay valid for the lifetime of the application.
 */
esp_err_t claw_endpoint_register_provider(const char *id, const claw_endpoint_provider_ops_t *ops);

/** @brief Check a configuration. NULL or empty provider_id selects the static provider. */
bool claw_endpoint_is_configured(const char *provider_id, const claw_endpoint_config_t *config);

esp_err_t claw_endpoint_create(const char *provider_id,
                               const claw_endpoint_config_t *config,
                               claw_endpoint_handle_t *ret_handle);
esp_err_t claw_endpoint_acquire(claw_endpoint_handle_t handle,
                                const claw_endpoint_request_t *request,
                                claw_endpoint_resolved_t *out);
/**
 * @brief Report how the service treated the credentials of `resolved`.
 *
 * @return For REJECTED, true when the caller should acquire again and retry
 *         once. Always false for ACCEPTED.
 */
bool claw_endpoint_report(claw_endpoint_handle_t handle,
                          const claw_endpoint_resolved_t *resolved,
                          claw_endpoint_outcome_t outcome);
/** Caller must finish all acquire/report/transport calls before deletion.
 * Any backend borrowing this handle must be destroyed first. */
void claw_endpoint_delete(claw_endpoint_handle_t handle);

/** Readable name for esp_err_t values including CLAW_ENDPOINT_ERR_*. */
const char *claw_endpoint_err_to_name(esp_err_t err);

/* Helpers for providers filling a claw_endpoint_resolved_t. */
esp_err_t claw_endpoint_resolved_set_url(claw_endpoint_resolved_t *resolved, const char *url);
esp_err_t claw_endpoint_resolved_add_header(claw_endpoint_resolved_t *resolved,
                                            const char *name,
                                            const char *value);
void claw_endpoint_resolved_free(claw_endpoint_resolved_t *resolved);

#ifdef __cplusplus
}
#endif
