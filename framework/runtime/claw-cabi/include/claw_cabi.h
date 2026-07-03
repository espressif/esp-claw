/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ============================================================================
 *  AUTHORITATIVE, hand-maintained header for the `claw-cabi` C ABI.
 *
 *  The Rust ABI is implemented (src/abi.rs, src/result.rs, src/wrappers.rs,
 *  src/lib.rs); the struct layouts and enum discriminants here match it exactly.
 *  This header is kept by hand rather than cbindgen-generated: cbindgen strips
 *  the prose below and emits enum constants as CLAW_CAPABILITY_ERROR_KIND_T_OK
 *  (a `_t`-qualified prefix) instead of the names used here. Run
 *  `cbindgen --config cbindgen.toml --crate claw-cabi` and compare *layout* to
 *  cross-check this header after changing the ABI. See DESIGN.md.
 *
 *  claw-cabi is the single OUTBOUND C ABI (Rust -> C) for the agent/capability
 *  stack. To C, the only concept is a "capability": one descriptor, one
 *  register call. The internal Tool/Channel/lifecycle-only split is a tagged
 *  union selected by `role` (see claw_capability_t), mirroring the Rust
 *  `CapabilityRole` enum so the mutually exclusive arms are structural.
 * ============================================================================
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max bytes a tool `execute` callback may write into the provided output buffer. */
#define CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY 4096

/* ------------------------------------------------------------------------- */
/* Result: OK, or an error kind + owned message (Rust's Result, C-shaped).   */
/* ------------------------------------------------------------------------- */

typedef enum {
    CLAW_CAPABILITY_OK = 0,          /* success; `message` is NULL            */
    CLAW_CAPABILITY_INVALID_ARGUMENT,
    CLAW_CAPABILITY_NOT_FOUND,       /* requested object not found             */
    CLAW_CAPABILITY_ALREADY_EXISTS,  /* requested object already exists        */
    CLAW_CAPABILITY_INVALID_STATE,
    CLAW_CAPABILITY_FAILED,          /* catch-all: panic guard / callback failure */
} claw_capability_error_kind_t;

/*
 * A Result value. `message` is ALWAYS borrowed; nobody frees it across the ABI,
 * and there is no *_free function. Copy it if you need to keep it.
 *
 *  - Returned BY claw-cabi functions (Rust -> C): structural errors point at
 *    static strings; CLAW_CAPABILITY_FAILED text lives in a Rust thread-local
 *    buffer valid only until your NEXT claw-cabi call on this thread. Read/copy
 *    it before then.
 *  - Returned BY your callbacks (C -> Rust): Rust copies `message` synchronously
 *    and never frees it. You keep ownership; it only needs to stay valid until
 *    your callback returns (a string literal, static buffer, or user_context
 *    buffer all work).
 */
typedef struct {
    claw_capability_error_kind_t kind;
    const char *message;             /* NULL when kind == CLAW_CAPABILITY_OK; borrowed */
} claw_capability_result_t;

static inline bool claw_capability_is_ok(claw_capability_result_t result)
{
    return result.kind == CLAW_CAPABILITY_OK;
}

/* ------------------------------------------------------------------------- */
/* Callbacks. All return claw_capability_result_t with a BORROWED message.   */
/* ------------------------------------------------------------------------- */

/* Resource lifecycle. Any hook may be NULL.
 * Order: init (once) -> start -> stop (per enable/disable) -> deinit (once). */
typedef claw_capability_result_t (*claw_capability_lifecycle_callback_t)(void *user_context);

typedef struct {
    claw_capability_lifecycle_callback_t init;
    claw_capability_lifecycle_callback_t start;
    claw_capability_lifecycle_callback_t stop;
    claw_capability_lifecycle_callback_t deinit;
} claw_capability_lifecycle_t;

/* A model-callable tool. Write up to `output_capacity` bytes into
 * `output_buffer`, set `*output_length` to bytes written and `*output_success`
 * to the tool's own success flag. Output exceeding
 * CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY must return CLAW_CAPABILITY_FAILED. */
typedef claw_capability_result_t (*claw_capability_execute_callback_t)(const char *arguments_json,
                                                                       char *output_buffer,
                                                                       size_t output_capacity,
                                                                       size_t *output_length,
                                                                       bool *output_success,
                                                                       void *user_context);

typedef struct claw_channel_runtime claw_channel_runtime_t;

/* Channel open receives an owned runtime handle. C must eventually destroy it
 * with claw_channel_runtime_destroy, normally from the matching close path. */
typedef claw_capability_result_t (*claw_capability_channel_open_callback_t)(claw_channel_runtime_t *runtime,
                                                                            void *user_context);

typedef claw_capability_result_t (*claw_capability_channel_close_callback_t)(void *user_context);

/* Outbound delivery for a message channel. `reply_to_message_id` may be NULL. */
typedef claw_capability_result_t (*claw_capability_send_callback_t)(const char *channel,
                                                                    const char *chat_id,
                                                                    const char *text,
                                                                    const char *reply_to_message_id,
                                                                    void *user_context);

/* ------------------------------------------------------------------------- */
/* Capability descriptor — a tagged union over the mutually exclusive role.  */
/* ------------------------------------------------------------------------- */

/* Which arm of the role union is live. Mirrors the Rust `CapabilityRole`. */
typedef enum {
    CLAW_CAPABILITY_ROLE_NONE = 0,   /* lifecycle-only service; no payload     */
    CLAW_CAPABILITY_ROLE_TOOL,       /* model-callable tool                    */
    CLAW_CAPABILITY_ROLE_CHANNEL,    /* message channel                        */
} claw_capability_role_t;

/* TOOL payload: both fields required. */
typedef struct {
    const char *schema_json;
    claw_capability_execute_callback_t execute;
} claw_capability_tool_t;

/* CHANNEL payload. */
typedef struct {
    claw_capability_channel_open_callback_t open;
    claw_capability_channel_close_callback_t close;
    claw_capability_send_callback_t send;
} claw_capability_channel_t;

/*
 * `role` selects the live union arm and is validated against it:
 *  - ROLE_TOOL    -> role_data.tool.{execute, schema_json} required
 *  - ROLE_CHANNEL -> role_data.channel.{open, close, send} required
 *  - ROLE_NONE    -> no payload; lifecycle MUST have at least one hook set
 * Any violation (incl. an empty `id`) is CLAW_CAPABILITY_INVALID_ARGUMENT.
 * Setting "both Tool and Channel" is structurally impossible — the arms share
 * storage and only the one named by `role` is read.
 */
typedef struct {
    const char *id;                  /* required, unique                       */
    const char *description;         /* nullable                               */
    claw_capability_role_t role;     /* selects the union arm below            */
    union {
        claw_capability_tool_t    tool;     /* role == CLAW_CAPABILITY_ROLE_TOOL    */
        claw_capability_channel_t channel;  /* role == CLAW_CAPABILITY_ROLE_CHANNEL */
    } role_data;                     /* unused when role == CLAW_CAPABILITY_ROLE_NONE */
    claw_capability_lifecycle_t lifecycle;  /* orthogonal; hooks may individually be NULL */
    void *user_context;              /* passed to every callback above         */
} claw_capability_t;

/* A registrable bundle sharing one optional group lifecycle. */
typedef struct {
    const char *id;
    const claw_capability_t *members;
    size_t member_count;
    claw_capability_lifecycle_t lifecycle;  /* shared; hooks may be NULL       */
    void *user_context;
} claw_capability_group_t;

/* ------------------------------------------------------------------------- */
/* Control plane: registration.                                              */
/*                                                                           */
/* The registry is created and owned on the Rust side; a handle is passed to */
/* C (e.g. into each component's `capability_xxx_register(registry)`) purely  */
/* so C can register into it. Lifecycle *driving* (start/stop/enable/disable/ */
/* unregister), queries, and building the agent runtime are all done from     */
/* Rust and are intentionally NOT part of this ABI.                          */
/* ------------------------------------------------------------------------- */

typedef struct claw_capability_registry claw_capability_registry_t;

claw_capability_result_t claw_capability_registry_create(claw_capability_registry_t **ret_registry);
claw_capability_result_t claw_capability_registry_destroy(claw_capability_registry_t *registry);
claw_capability_result_t claw_capability_register(claw_capability_registry_t *registry,
                                                  const claw_capability_t *capability);
claw_capability_result_t claw_capability_register_group(claw_capability_registry_t *registry,
                                                        const claw_capability_group_t *group);

/*
 * Invoke a registered TOOL-role capability by name, synchronously — the C-facing
 * "call a capability by name" seam (the replacement for the old claw_cap_call).
 *
 * For non-agent callers that must run a capability directly rather than through
 * the agent's tool loop: the event router (call_cap / run_script / send_message)
 * and Lua `capability.call`. Only synchronous tools are callable here; every
 * C-registered tool is synchronous, so this covers all C capabilities.
 *
 * On success the tool's output text is copied into `output_buffer` (NUL
 * terminated), `*output_length` is set to the output byte length (excluding the
 * NUL), and `*output_success` is set to the tool's own success flag. `arguments_json`
 * may be NULL (treated as "{}"). An unknown capability returns
 * CLAW_CAPABILITY_NOT_FOUND; output that does not fit `output_capacity` returns
 * CLAW_CAPABILITY_FAILED with `*output_length` set to the required byte length.
 */
claw_capability_result_t claw_capability_invoke(claw_capability_registry_t *registry,
                                                const char *cap_name,
                                                const char *arguments_json,
                                                char *output_buffer,
                                                size_t output_capacity,
                                                size_t *output_length,
                                                bool *output_success);

/* ------------------------------------------------------------------------- */
/* Data plane: inbound channel messages.                                     */
/*                                                                           */
/* Registered C channel capabilities receive a claw_channel_runtime_t in      */
/* their open callback and use claw_channel_runtime_push(). App-level         */
/* producers such as the event router can submit directly through             */
/* claw_agent_system_push_message(). Both paths require an explicit           */
/* claw_agent_system_session_bind() for the message's channel/chat_id.        */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char *message_id;
    const char *channel;
    const char *chat_id;
    const char *sender_id;           /* nullable */
    const char *text;
} claw_inbound_message_t;

claw_capability_result_t claw_channel_runtime_push(claw_channel_runtime_t *runtime,
                                                   const claw_inbound_message_t *message);
claw_capability_result_t claw_channel_runtime_destroy(claw_channel_runtime_t *runtime);

/* ------------------------------------------------------------------------- */
/* Agent runtime: ESP-IDF target creates and owns the Rust AgentSystem.       */
/*                                                                           */
/* Expected boot order for C firmware:                                        */
/*   1. claw_capability_registry_create(&registry)                            */
/*   2. register every capability/group into registry                         */
/*   3. claw_agent_system_create(&config, registry, &system)                  */
/*   4. claw_agent_system_start(system)                                       */
/*   5. create + bind sessions, then push channel messages                    */
/*                                                                           */
/* Destroy in reverse order: stop/destroy system, then destroy registry when  */
/* no component will register into it anymore.                                */
/* ------------------------------------------------------------------------- */

typedef struct claw_agent_system claw_agent_system_t;

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *persistence_dir;    /* required DATA-rooted directory */
} claw_agent_system_config_t;

claw_capability_result_t claw_agent_system_create(const claw_agent_system_config_t *config,
                                                  claw_capability_registry_t *registry,
                                                  claw_agent_system_t **ret_system);
claw_capability_result_t claw_agent_system_start(claw_agent_system_t *system);
claw_capability_result_t claw_agent_system_stop(claw_agent_system_t *system);
claw_capability_result_t claw_agent_system_destroy(claw_agent_system_t *system);
claw_capability_result_t claw_agent_system_push_message(claw_agent_system_t *system,
                                                        const claw_inbound_message_t *message);

typedef struct {
    const char *session_id;          /* borrowed; valid only during callback */
    const char *channel;             /* nullable; borrowed */
    const char *chat_id;             /* nullable; borrowed */
} claw_agent_session_record_t;

typedef claw_capability_result_t (*claw_agent_session_list_callback_t)(
    const claw_agent_session_record_t *record,
    void *user_context);

/*
 * Explicit session lifecycle.
 *
 * `claw_agent_system_session_create` writes the new session id ("session-N")
 * into `session_id_buffer` and sets `session_id_length` to the required byte
 * length, excluding the trailing NUL. `session_id_capacity` must be at least
 * 32 bytes. The function returns CLAW_CAPABILITY_FAILED before creating a
 * session if the buffer is too small.
 *
 * Inbound channel messages are accepted only after an explicit
 * claw_agent_system_session_bind(system, session_id, channel, chat_id).
 *
 * `claw_agent_system_session_delete` removes the session and drops its live
 * agent graph. Deleting an unknown session returns CLAW_CAPABILITY_NOT_FOUND.
 */
claw_capability_result_t claw_agent_system_session_create(claw_agent_system_t *system,
                                                          char *session_id_buffer,
                                                          size_t session_id_capacity,
                                                          size_t *session_id_length);
claw_capability_result_t claw_agent_system_session_bind(claw_agent_system_t *system,
                                                        const char *session_id,
                                                        const char *channel,
                                                        const char *chat_id);
claw_capability_result_t claw_agent_system_session_list(claw_agent_system_t *system,
                                                        claw_agent_session_list_callback_t callback,
                                                        void *user_context);
claw_capability_result_t claw_agent_system_session_delete(claw_agent_system_t *system,
                                                          const char *session_id);

#ifdef __cplusplus
}
#endif
