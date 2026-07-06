/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Required non-null UTF-8 C string. */
    const char *api_key;
    /* Required non-null UTF-8 C string, e.g. "openai_compatible". */
    const char *backend_type;
    /* Required non-null UTF-8 C string. */
    const char *model;
    /* Required non-null UTF-8 C string. */
    const char *base_url;
    /* Required non-null UTF-8 C string. */
    const char *persistence_dir;
    /* Optional UTF-8 C string; may be NULL. Writable DATA skills root, e.g.
     * "<DATA>/skills". Scanned first so it takes priority over the system root. */
    const char *skills_root_dir;
    /* Optional UTF-8 C string; may be NULL. Read-only firmware skills root,
     * e.g. "/system/skills". Scanned after the DATA root. */
    const char *system_skills_root_dir;
} claw_agent_config_t;

typedef enum {
    CLAW_AGENT_RESPONSE_STATUS_OK = 0,
    CLAW_AGENT_RESPONSE_STATUS_ERROR = 1,
} claw_agent_response_status_t;

typedef struct {
    claw_agent_response_status_t status;
    /* Owned UTF-8 C string; release with claw_agent_session_response_free. */
    char *text;
    /* Owned UTF-8 C string; release with claw_agent_session_response_free. */
    char *error_message;
} claw_agent_response_t;

/*
 * Initialize the runtime config.
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if config is NULL or any required string is NULL,
 *   non-UTF-8, or backend_type is unknown.
 * - ESP_ERR_INVALID_STATE if the runtime is already initialized.
 */
esp_err_t claw_agent_init(const claw_agent_config_t *config);

/*
 * Start the runtime worker.
 *
 * Returns:
 * - ESP_OK on success or if already started.
 * - ESP_ERR_INVALID_STATE if the runtime was not initialized.
 * - ESP_FAIL for worker/thread/agent startup failures.
 */
esp_err_t claw_agent_start(void);

/*
 * Stop the runtime worker after in-flight requests drain.
 *
 * Returns:
 * - ESP_OK on success or if initialized but not running.
 * - ESP_ERR_INVALID_STATE if the runtime was not initialized or the worker is gone.
 * - ESP_FAIL for worker/tool shutdown failures.
 */
esp_err_t claw_agent_stop(void);

/*
 * Stop the runtime worker if needed and release runtime state.
 *
 * Returns:
 * - ESP_OK on success or if already deinitialized.
 * - ESP_ERR_INVALID_STATE if the running worker cannot be stopped cleanly.
 * - ESP_FAIL for worker/tool shutdown failures.
 */
esp_err_t claw_agent_deinit(void);

/*
 * Enqueue input for an explicit numeric session id.
 *
 * session_id must be non-zero and refer to a live session returned by
 * claw_agent_session_create().
 *
 * text must be a non-NULL UTF-8 string.
 *
 * out_request_id may be NULL. If NULL, the request is fire-and-forget and no
 * response is retained for claw_agent_session_receive. If non-NULL, it is
 * written only on ESP_OK and the completed response can be received once.
 *
 * Returns:
 * - ESP_OK after the worker accepts and schedules the request.
 * - ESP_ERR_INVALID_ARG for invalid text/session arguments.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_ERR_NOT_FOUND if session_id is not live.
 * - ESP_FAIL for unexpected scheduling failures.
 */
esp_err_t claw_agent_session_submit(uint32_t session_id,
                                    const char *text,
                                    uint32_t *out_request_id);

/*
 * Create a new numeric session id.
 *
 * out_session_id must be non-NULL.
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if out_session_id is NULL.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 */
esp_err_t claw_agent_session_create(uint32_t *out_session_id);

/*
 * List live numeric session ids.
 *
 * out_count must be non-NULL. On every successful or ESP_ERR_INVALID_SIZE
 * return, out_count receives the total live session count. out_session_ids may
 * be NULL only when capacity is 0.
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if out_count is NULL, or out_session_ids is NULL while
 *   capacity is non-zero.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_ERR_INVALID_SIZE if capacity is smaller than the live session count.
 */
esp_err_t claw_agent_session_list(uint32_t *out_session_ids,
                                  size_t capacity,
                                  size_t *out_count);

/*
 * Delete a live numeric session id.
 *
 * session_id must be non-zero. Deleting a session also drops its live agent
 * graph.
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if session_id is 0.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_ERR_NOT_FOUND if session_id is not live.
 */
esp_err_t claw_agent_session_delete(uint32_t session_id);

/*
 * Wait for the completed output of a previous submit request.
 *
 * session_id must be non-zero and match the session used for submit.
 * request_id must be non-zero and must come from a successful submit call
 * whose out_request_id was non-NULL. out_response must be non-NULL. On
 * success, out_response owns text and error_message until
 * claw_agent_session_response_free. A response can be received only once.
 *
 * timeout_ms == 0 performs a non-blocking poll. Unknown request ids and
 * not-yet-completed requests both return ESP_ERR_TIMEOUT when the timeout
 * expires.
 *
 * Returns:
 * - ESP_OK with out_response populated.
 * - ESP_ERR_INVALID_ARG if session_id/request_id is 0, session_id does not
 *   match request_id, or out_response is NULL.
 * - ESP_ERR_INVALID_STATE if the runtime is not initialized.
 * - ESP_ERR_TIMEOUT if the response is not available before timeout_ms.
 * - ESP_FAIL for unexpected response allocation failures.
 */
esp_err_t claw_agent_session_receive(uint32_t session_id,
                                     uint32_t request_id,
                                     claw_agent_response_t *out_response,
                                     uint32_t timeout_ms);

/*
 * Free owned strings returned by claw_agent_session_receive.
 *
 * response may be NULL. After return, response->text and
 * response->error_message are set to NULL.
 */
void claw_agent_session_response_free(claw_agent_response_t *response);

#ifdef __cplusplus
}
#endif
