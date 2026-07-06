/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "claw_cap.h"
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

typedef struct {
    /* Required non-null UTF-8 C string. */
    const char *text;
    /* Optional UTF-8 C string; may be NULL. */
    const char *source_cap;
    /* Optional UTF-8 C string; may be NULL. */
    const char *source_channel;
    /* Optional UTF-8 C string; may be NULL. */
    const char *source_chat_id;
    /* Optional UTF-8 C string; may be NULL. */
    const char *target_channel;
    /* Optional UTF-8 C string; may be NULL. */
    const char *target_chat_id;
} claw_agent_input_t;

typedef enum {
    CLAW_AGENT_RESPONSE_STATUS_OK = 0,
    CLAW_AGENT_RESPONSE_STATUS_ERROR = 1,
} claw_agent_response_status_t;

typedef struct {
    uint32_t request_id;
    claw_agent_response_status_t status;
    /* Owned UTF-8 C string; release with claw_agent_response_free. */
    char *text;
    /* Owned UTF-8 C string; release with claw_agent_response_free. */
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
 * Enqueue input and return after scheduling; does not wait for model output.
 *
 * input must be non-NULL. input->text must be non-NULL UTF-8. Other input
 * string fields may be NULL.
 *
 * out_request_id may be NULL. If NULL, the request is fire-and-forget and no
 * response is retained for claw_agent_receive. If non-NULL, it is written only
 * on ESP_OK and the completed response can be received once.
 *
 * Returns:
 * - ESP_OK after the worker accepts and schedules the request.
 * - ESP_ERR_INVALID_ARG for invalid input pointers or strings.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_FAIL for unexpected scheduling failures.
 */
esp_err_t claw_agent_submit(const claw_agent_input_t *input, uint32_t *out_request_id);

/*
 * Wait for the completed output of a previous submit request.
 *
 * request_id must be non-zero and must come from a successful submit call whose
 * out_request_id was non-NULL. out_response must be non-NULL. On success,
 * out_response owns text and error_message until claw_agent_response_free.
 * A response can be received only once.
 *
 * timeout_ms == 0 performs a non-blocking poll. Unknown request ids and
 * not-yet-completed requests both return ESP_ERR_TIMEOUT when the timeout
 * expires.
 *
 * Returns:
 * - ESP_OK with out_response populated.
 * - ESP_ERR_INVALID_ARG if request_id is 0 or out_response is NULL.
 * - ESP_ERR_INVALID_STATE if the runtime is not initialized.
 * - ESP_ERR_TIMEOUT if the response is not available before timeout_ms.
 * - ESP_FAIL for unexpected response allocation failures.
 */
esp_err_t claw_agent_receive(uint32_t request_id,
                              claw_agent_response_t *out_response,
                              uint32_t timeout_ms);

/*
 * Free owned strings returned by claw_agent_receive.
 *
 * response may be NULL. After return, response->text and
 * response->error_message are set to NULL.
 */
void claw_agent_response_free(claw_agent_response_t *response);

/*
 * Capability entry point (matches claw_cap_execute_fn) that submits one inbound
 * message to the running agent. Reads "text" from input_json and routing fields
 * from ctx (channel, chat_id, target_channel, target_chat_id, source_cap), then
 * schedules a fire-and-forget request. On success writes "request_id=<n>" to
 * output.
 *
 * Register this through claw_cap so the event router and other subsystems can
 * invoke the agent via claw_cap_call instead of a direct callback.
 *
 * Returns:
 * - ESP_OK after the worker accepts and schedules the request.
 * - ESP_ERR_INVALID_ARG for malformed input_json.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_FAIL for unexpected scheduling failures.
 */
esp_err_t claw_agent_cap_execute(const char *input_json,
                                 const claw_cap_call_context_t *ctx,
                                 char *output,
                                 size_t output_size);

#ifdef __cplusplus
}
#endif
