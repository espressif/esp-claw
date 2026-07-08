# claw-core Trace

This document defines the `claw-core` trace vocabulary. Large concepts are spans;
facts inside those concepts are events.

## Session

### Tracing Context

span-name: `session`

### Events

`opened`: Session was opened. The event stream is attached and can receive session events.
`open_rejected`: Opening the session failed because it was missing, already open, or the worker stopped.
`submit_accepted`: User input was accepted and queued for a turn.
`submit_rejected`: User input was rejected because the session was closed or busy.
`control_requested`: Interrupt or cancel was accepted for the session.
`control_rejected`: Interrupt or cancel was rejected because the session was closed.
`close_requested`: Close was accepted. The engine starts cancellation and cleanup.
`closed`: Session close completed. The event stream receives `Closed` and runtime state is removed.

## Session Create

### Tracing Context

span-name: `session.create`

### Events

`created`: Session id was created.

## Session Delete

### Tracing Context

span-name: `session.delete`

### Events

`registry_removed`: Session id was removed from the registry.
`runtime_state_removed`: Session drive and agent instance state were removed.
`delete_rejected`: Delete/close found no live session to remove.

## Turn

### Tracing Context

span-name: `turn`

### Events

`input_delivered`: User input was delivered to the root agent.
`background_result`: Background subagent work made the root ready again.
`approval_resolved`: User reply resolved a pending approval.
`approval_clarification`: Approval resolver asked the user for clarification.
`output`: Root-visible text was emitted to the session stream.
`error`: Turn drive failed and emitted a session error.
`cancelled_cleanup`: A cancelled turn ran cleanup before ending.

## Agent Factory

### Tracing Context

span-name: `agent.factory`
span-name: `agent.create`

### Events

`missing_persistence_dir`: Factory construction rejected an empty persistence root.
`extraction_llm_init_failed`: Internal extraction LLM client failed to initialize.
`long_term_memory_init_failed`: Long-term memory failed to initialize.
`skill_root_missing`: Configured skill root did not exist and was skipped.
`skill_scan_failed`: Skill root scan failed and filesystem skills were disabled.
`unknown_kind`: Agent kind had no baked manifest.
`unknown_tool`: Manifest referenced a tool that is not available.
`transcript_open_failed`: Agent transcript store could not be opened.
`agent_build_failed`: Agent object could not be built.
`context_adapter_attach_failed`: Profile or long-term memory context adapter could not be attached.
`goal_seed_failed`: Initial goal could not be appended to the agent.
`created`: Agent was built and returned to the instance.

## Agent

### Tracing Context

span-name: `agent`

### Events

`awaiting_approval`: Agent parked on a human approval request.
`approval_resolved`: Pending approval was resolved from a user reply.
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

## Iteration

### Tracing Context

span-name: `iteration_loop`

### Events

`completed`: LLM produced final text without tool calls.
`preempted`: Iteration stopped at an interrupt checkpoint.
`chat_failed`: LLM chat failed for a non-interrupt reason.
`tool_calls`: LLM requested one or more tool calls.
`assistant_tool_calls_invalid`: Assistant tool-call message was missing, malformed, or could not be appended.
`tool_round_completed`: Tool round completed and produced tool messages.
`tool_round_failed`: Tool round failed before a valid patch could be produced.

## Skill Related

### Tracing Context

span-name: `skill.catalog`

### Events

`root_missing`: Skill root directory was missing and skipped.
`scan_failed`: Skill root scan failed and filesystem skills were disabled.
`manifest_ids_catalog_only`: Manifest skill ids were observed but the full shared catalog was projected.

## Tool Related

### Tracing Context

span-name: `toolcall`

### Events

`arguments`: Tool arguments were recorded.
`parse_failed`: Tool invocation could not be parsed from the model call.
`result`: Tool completed, was blocked, or requested approval. Carries `ok` and `blocked`.
`preempted`: Interrupt was observed before the tool call ran.
`spawn_kind_rejected`: `spawn_subagent` rejected a kind outside the caller's allowed kinds.
`spawn_unknown_kind_rejected`: `spawn_subagent` rejected a kind without a baked manifest.
