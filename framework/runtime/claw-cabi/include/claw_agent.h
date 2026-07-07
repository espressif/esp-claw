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
    /* Content events (non-terminal). `text` is an append fragment: concatenate
     * across events to reconstruct the full string. */
    CLAW_AGENT_EVENT_KIND_OUTPUT = 0,    /* assistant-visible answer text */
    CLAW_AGENT_EVENT_KIND_REASONING = 1, /* model thinking text (truncated) */
    CLAW_AGENT_EVENT_KIND_TOOLS = 2,     /* comma-joined tool names of a round */
    /* Terminal events: after either, the request id is consumed and further
     * receives return ESP_ERR_TIMEOUT. */
    CLAW_AGENT_EVENT_KIND_DONE = 3,  /* turn finished successfully */
    CLAW_AGENT_EVENT_KIND_ERROR = 4, /* turn failed; see error_message */
} claw_agent_event_kind_t;

typedef struct {
    claw_agent_event_kind_t kind;
    /* Owned UTF-8 fragment for content events (OUTPUT/REASONING/TOOLS); NULL for
     * DONE/ERROR. Release with claw_agent_event_free. */
    char *text;
    /* Owned UTF-8 message for ERROR; NULL otherwise. Release with
     * claw_agent_event_free. */
    char *error_message;
} claw_agent_event_t;

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
 * Request graceful interruption of a submitted turn.
 *
 * The request is keyed by the request id returned from
 * claw_agent_session_submit(), so a late interrupt cannot affect a newer
 * submission in the same session. The stream may not end immediately; keep
 * receiving until DONE or ERROR.
 *
 * Returns:
 * - ESP_OK if the request was accepted or already unnecessary.
 * - ESP_ERR_INVALID_ARG for invalid ids.
 * - ESP_ERR_INVALID_STATE if the runtime is not started.
 * - ESP_ERR_NOT_FOUND if request_id is no longer pending.
 */
esp_err_t claw_agent_session_interrupt(uint32_t session_id,
                                       uint32_t request_id);

/*
 * Request hard cancellation of a submitted turn.
 *
 * The request is keyed by the request id returned from
 * claw_agent_session_submit(), so a late cancel cannot affect a newer
 * submission in the same session. The stream may not end immediately; keep
 * receiving until DONE or ERROR.
 *
 * Returns:
 * - ESP_OK if the request was accepted or already unnecessary.
 * - ESP_ERR_INVALID_ARG for invalid ids.
 * - ESP_ERR_INVALID_STATE if the runtime is not started.
 * - ESP_ERR_NOT_FOUND if request_id is no longer pending.
 */
esp_err_t claw_agent_session_cancel(uint32_t session_id,
                                    uint32_t request_id);

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
 * Receive the next event of a submitted turn, one event per call.
 *
 * A turn is consumed incrementally: call this in a loop, handling each event as
 * it arrives (content events stream out while the turn is still running), until
 * a terminal event (CLAW_AGENT_EVENT_KIND_DONE or _ERROR) is delivered.
 *
 * session_id must be non-zero and match the session used for submit. request_id
 * must be non-zero and must come from a successful submit call whose
 * out_request_id was non-NULL. out_event must be non-NULL. On ESP_OK, out_event
 * owns text/error_message until claw_agent_event_free.
 *
 * timeout_ms == 0 performs a non-blocking poll (returns the next buffered event
 * or ESP_ERR_TIMEOUT immediately). Otherwise it waits up to timeout_ms for the
 * next event; on timeout the turn is retained and a later call resumes it.
 * Unknown/consumed request ids also return ESP_ERR_TIMEOUT.
 *
 * Returns:
 * - ESP_OK with out_event populated (inspect out_event->kind).
 * - ESP_ERR_INVALID_ARG if session_id/request_id is 0, session_id does not
 *   match request_id, or out_event is NULL.
 * - ESP_ERR_INVALID_STATE if the runtime is not initialized.
 * - ESP_ERR_TIMEOUT if no event is available before timeout_ms.
 * - ESP_FAIL for unexpected event allocation failures.
 */
esp_err_t claw_agent_session_receive(uint32_t session_id,
                                     uint32_t request_id,
                                     claw_agent_event_t *out_event,
                                     uint32_t timeout_ms);

/*
 * Free owned strings returned by claw_agent_session_receive.
 *
 * event may be NULL. After return, event->text and event->error_message are set
 * to NULL.
 */
void claw_agent_event_free(claw_agent_event_t *event);

#ifdef __cplusplus
}
#endif
