# claw-core Trace

This document defines the `claw-core` trace vocabulary on top of
`claw-log`'s flat-tree trace format. `claw-log` owns the line grammar
(`tracing-context`, `incremental-context`, and `custom-context`); this file owns
the `claw-core` span names, event names, incremental `run.*` keys, and custom
context fields.

Trace user data by shape, not by payload. Do not emit raw user text, captions,
attachment file paths, file contents, tool arguments, or model output text. For
messages and attachments, prefer fields such as `has_text`, `text_bytes`,
`attachment_count`, and `attachment_kinds`.

## Levels

`claw-core` trace vocabulary uses only `info`, `warn`, and `error`.

`info`: Expected lifecycle, progress, and successful state transitions.
`warn`: A request was rejected, optional capability degraded, work was cancelled
or preempted, or a policy/model/tool issue was handled without crashing the
runtime.
`error`: The current operation failed, returned or surfaced an error, or
requested work was dropped because it could not be constructed or driven.

Context-carrying spans (`session`, `turn`, `agent`, `iteration_loop`,
`toolcall`, etc.) use `info_span!` so `info`/`warn`/`error` events retain their
incremental context when the runtime level is `Info`.

## Session

### Tracing Context

span-name: `session`

### Incremental Context

`run.session`: Session id.

### Events

`opened`: Session was opened. The event stream is attached and can receive session events.
`open_rejected`: Opening the session failed because it was missing, already open, or the worker stopped.
`submit_accepted`: User input was accepted and queued for a turn.
`submit_rejected`: User input was rejected because the session was closed or busy.
`control_requested`: Interrupt or cancel was accepted for the session.
`control_rejected`: Interrupt or cancel was rejected because the session was closed.
`close_requested`: Close was accepted. The engine starts stream shutdown and cancellation if work is live.
`close_rejected`: Close failed because the session was missing or not open.
`closed`: Session stream close completed. The event stream receives `Closed`; the session id remains live unless a delete requested removal.

### Event Fields

`open_rejected`: `reason`.
`submit_accepted`: `has_text`, `text_bytes`, `attachment_count`, `attachment_kinds`.
`submit_rejected`: `reason`, `has_text`, `text_bytes`, `attachment_count`, `attachment_kinds`.
`control_requested`: `op`.
`control_rejected`: `op`, `reason`.
`close_rejected`: `reason`.

## Session Create

### Tracing Context

span-name: `session.create`

### Events

`created`: Session id was created.

### Event Fields

`created`: `session`.

## Session Delete

### Tracing Context

span-name: `session.delete`

### Incremental Context

`run.session`: Session id being removed.

### Events

`registry_removed`: Session id was removed from the registry.
`runtime_state_removed`: Session drive and agent instance state were removed.
`delete_requested`: Delete was accepted.
`delete_rejected`: Delete found no live session to remove.

### Event Fields

`delete_rejected`: `reason`.

## Turn

### Tracing Context

span-name: `turn`

### Incremental Context

`run.turn`: Session-local turn id.

### Span Fields

`cause`: Why this turn is being driven.

### Events

`input_delivered`: User input was delivered to the root agent.
`background_result`: Background subagent work made the root ready again.
`approval_resolved`: User reply resolved a pending approval.
`approval_clarification`: Approval resolver asked the user for clarification.
`output`: Root-visible text was emitted to the session stream.
`error`: Turn drive failed and emitted a session error.
`cancelled_cleanup`: A cancelled turn ran cleanup before ending.

### Event Fields

`input_delivered`: `has_text`, `text_bytes`, `attachment_count`, `attachment_kinds`.
`approval_resolved`: `decision`, optionally `approval`.
`approval_clarification`: `reason`.
`output`: `text_bytes`.
`error`: `kind`.

## Agent Factory

### Tracing Context

span-name: `agent.factory`

### Events

`missing_persistence_dir`: Factory construction rejected an empty persistence root.
`extraction_llm_init_failed`: Internal extraction LLM client failed to initialize.
`long_term_memory_init_failed`: Long-term memory failed to initialize.

### Event Fields

`missing_persistence_dir`: `reason`.
`extraction_llm_init_failed`: `kind`.
`long_term_memory_init_failed`: `kind`.

## Agent Create

### Tracing Context

span-name: `agent.create`

### Events

`unknown_kind`: Agent kind had no baked manifest.
`unknown_tool`: Manifest referenced a tool that is not available.
`transcript_open_failed`: Agent transcript store could not be opened.
`agent_build_failed`: Agent object could not be built.
`context_adapter_attach_failed`: Profile or long-term memory context adapter could not be attached.
`goal_seed_failed`: Initial goal could not be appended to the agent.
`manifest_ids_catalog_only`: Manifest skill ids were observed but the full shared catalog was projected.
`created`: Agent was built and returned to the instance.

### Event Fields

`unknown_kind`: `kind`.
`unknown_tool`: `kind`, `tool`.
`transcript_open_failed`: `agent`, `kind`.
`agent_build_failed`: `agent`, `kind`.
`context_adapter_attach_failed`: `agent`, `adapter`, `kind`.
`goal_seed_failed`: `agent`, `kind`.
`manifest_ids_catalog_only`: `count`.
`created`: `agent`, `kind`.

## Agent

### Tracing Context

span-name: `agent`

### Incremental Context

`run.agent`: Agent id for this subtree.

### Span Fields

`kind`: Agent kind.
`depth`: Agent depth relative to the root agent.

### Events

`awaiting_approval`: Agent parked on a human approval request.
`spawn_materialized`: Requested subagent was built and inserted into the graph.
`spawn_dropped`: Requested subagent could not be built or its parent was gone.
`delete_ignored`: Agent delete request targeted a non-descendant and was ignored.
`result_to_parent`: Subagent result was delivered or queued for its parent.
`manual_yielded`: Manual subagent yielded and stayed alive idle.
`root_cancelled`: Root task was cancelled.
`subagent_cancelled`: Subagent task was cancelled and removed.
`subtree_deleted`: Agent subtree was removed from registry, graph, queues, and approvals.
`tool_gate_blocked`: Tool gate blocked one or more tool calls.
`task_failed`: Agent task failed and returned to idle.
`preempt_patch_dropped`: Preempted partial patch had unmatched tool calls and was dropped.

### Event Fields

`awaiting_approval`: `approval`.
`spawn_materialized`: `parent_agent`, `child_agent`, `kind`.
`spawn_dropped`: `parent_agent`, `kind`, `reason`.
`delete_ignored`: `target_agent`, `reason`.
`result_to_parent`: `parent_agent`, `child_agent`, `queued`.
`root_cancelled`: `reason`.
`subagent_cancelled`: `agent`, `reason`.
`subtree_deleted`: `root_agent`, `count`.
`tool_gate_blocked`: `count`.
`preempt_patch_dropped`: `tool_call_count`.

## Iteration

### Tracing Context

span-name: `iteration_loop`

### Incremental Context

`run.iteration`: Iteration id.

### Events

`completed`: LLM produced final text without tool calls.
`preempted`: Iteration stopped at an interrupt checkpoint.
`chat_failed`: LLM chat failed for a non-interrupt reason.
`tool_calls`: LLM requested one or more tool calls.
`assistant_tool_calls_invalid`: Assistant tool-call message was missing, malformed, or could not be appended.
`tool_round_completed`: Tool round completed and produced tool messages.
`tool_round_failed`: Tool round failed before a valid patch could be produced.

### Event Fields

`completed`: `output_bytes`.
`preempted`: `checkpoint`.
`chat_failed`: `kind`.
`tool_calls`: `count`.
`assistant_tool_calls_invalid`: `kind`.
`tool_round_completed`: `count`.
`tool_round_failed`: `kind`.

## Skill Related

### Tracing Context

span-name: `skill.catalog`

### Events

`root_missing`: Skill root directory was missing and skipped.
`scan_failed`: Skill root scan failed and filesystem skills were disabled.

### Event Fields

`scan_failed`: `kind`.

## Tool Related

### Tracing Context

span-name: `toolcall`

### Span Fields

`tool`: Tool name. Use `none` as a placeholder when no tool was called.

### Events

`arguments`: Tool argument metadata was recorded.
`parse_failed`: Tool invocation could not be parsed from the model call.
`result`: Tool completed, was blocked, or requested approval.
`preempted`: Interrupt was observed before the tool call ran.
`spawn_kind_rejected`: `spawn_subagent` rejected a kind outside the caller's allowed kinds.
`spawn_unknown_kind_rejected`: `spawn_subagent` rejected a kind without a baked manifest.

### Event Fields

`arguments`: `argument_bytes`.
`parse_failed`: `kind`.
`result`: `ok`, `blocked`.
`preempted`: `checkpoint`.
`spawn_kind_rejected`: `kind`.
`spawn_unknown_kind_rejected`: `kind`.
