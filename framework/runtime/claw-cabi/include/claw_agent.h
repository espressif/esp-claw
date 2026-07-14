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
    /* Content events. `text` is an append fragment: concatenate
     * across events to reconstruct the full string. */
    CLAW_AGENT_EVENT_KIND_OUTPUT = 0,    /* assistant-visible answer text */
    CLAW_AGENT_EVENT_KIND_REASONING = 1, /* model thinking text (truncated) */
    CLAW_AGENT_EVENT_KIND_TOOLS = 2,     /* one tool call name (emitted per call) */
    CLAW_AGENT_EVENT_KIND_DONE = 3,   /* one root-visible turn finished */
    CLAW_AGENT_EVENT_KIND_ERROR = 4,  /* session work failed; see error_message */
    CLAW_AGENT_EVENT_KIND_CLOSED = 5, /* session stream closed; terminal */
    CLAW_AGENT_EVENT_KIND_OUTPUT_END = 6,    /* current output stream ended */
    CLAW_AGENT_EVENT_KIND_REASONING_END = 7, /* current reasoning stream ended */
    CLAW_AGENT_EVENT_KIND_TOOLS_END = 8,     /* no more tool calls this iteration */
} claw_agent_event_kind_t;

typedef struct {
    claw_agent_event_kind_t kind;
    /* Owned UTF-8 fragment for content events (OUTPUT/REASONING/TOOLS); NULL for
     * *_END/DONE/ERROR. Release with claw_agent_event_free. */
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
 * Open a numeric session's event stream.
 *
 * session_id must be non-zero and refer to a live session returned by
 * claw_agent_session_create().
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG for invalid session arguments.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or the session is
 *   already open.
 * - ESP_ERR_NOT_FOUND if session_id is not live.
 */
esp_err_t claw_agent_session_open(uint32_t session_id);

/*
 * Submit input for an open numeric session id.
 *
 * session_id must be non-zero and already opened with claw_agent_session_open().
 *
 * text must be a non-NULL UTF-8 string.
 *
 * Returns:
 * - ESP_OK after the worker accepts the input.
 * - ESP_ERR_INVALID_ARG for invalid text/session arguments.
 * - ESP_ERR_INVALID_STATE if the runtime is not started, is stopping, or the
 *   session already has an active foreground submit.
 * - ESP_ERR_NOT_FOUND if session_id is not open.
 * - ESP_FAIL for unexpected scheduling failures.
 */
esp_err_t claw_agent_session_submit(uint32_t session_id, const char *text);

/*
 * Request graceful interruption of the active foreground turn.
 *
 * The stream may not emit DONE immediately; keep receiving session events.
 *
 * Returns:
 * - ESP_OK if the request was accepted or already unnecessary.
 * - ESP_ERR_INVALID_ARG for invalid session id.
 * - ESP_ERR_INVALID_STATE if the runtime is not started.
 * - ESP_ERR_NOT_FOUND if session_id is not open.
 */
esp_err_t claw_agent_session_interrupt(uint32_t session_id);

/*
 * Request hard cancellation of foreground and background work in a session.
 *
 * The stream may not emit DONE/CLOSED immediately; keep receiving session
 * events.
 *
 * Returns:
 * - ESP_OK if the request was accepted or already unnecessary.
 * - ESP_ERR_INVALID_ARG for invalid session id.
 * - ESP_ERR_INVALID_STATE if the runtime is not started.
 * - ESP_ERR_NOT_FOUND if session_id is not open.
 */
esp_err_t claw_agent_session_cancel(uint32_t session_id);

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
 * Close an open numeric session stream.
 *
 * session_id must be non-zero and open. Closing cancels live work associated
 * with the open stream and eventually yields CLAW_AGENT_EVENT_KIND_CLOSED. The
 * session id remains live and may be opened again.
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if session_id is 0.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_ERR_NOT_FOUND if session_id is not open.
 */
esp_err_t claw_agent_session_close(uint32_t session_id);

/*
 * Delete a live numeric session id.
 *
 * session_id must be non-zero and live. If the session has an open stream,
 * deletion cancels live work and eventually yields CLAW_AGENT_EVENT_KIND_CLOSED.
 *
 * Returns:
 * - ESP_OK on success.
 * - ESP_ERR_INVALID_ARG if session_id is 0.
 * - ESP_ERR_INVALID_STATE if the runtime is not started or is stopping.
 * - ESP_ERR_NOT_FOUND if session_id is not live.
 */
esp_err_t claw_agent_session_delete(uint32_t session_id);

/*
 * Receive the next event from an open session, one event per call.
 *
 * A session is consumed incrementally: call this in a loop, handling each event
 * as it arrives. CLAW_AGENT_EVENT_KIND_DONE means one turn ended; continue
 * receiving for future user submits or background results. _CLOSED is terminal.
 *
 * session_id must be non-zero and open. out_event must be non-NULL. On ESP_OK,
 * out_event owns text/error_message until claw_agent_event_free.
 *
 * timeout_ms == 0 performs a non-blocking poll (returns the next buffered event
 * or ESP_ERR_TIMEOUT immediately). Otherwise it waits up to timeout_ms for the
 * next event; on timeout the session stream is retained and a later call
 * resumes it.
 *
 * Returns:
 * - ESP_OK with out_event populated (inspect out_event->kind).
 * - ESP_ERR_INVALID_ARG if session_id is 0 or out_event is NULL.
 * - ESP_ERR_INVALID_STATE if the runtime is not initialized.
 * - ESP_ERR_NOT_FOUND if session_id is not open.
 * - ESP_ERR_TIMEOUT if no event is available before timeout_ms.
 * - ESP_FAIL for unexpected event allocation failures.
 */
esp_err_t claw_agent_session_receive(uint32_t session_id,
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
