# Runtime Behavior Divergence vs master

This note compares the current `framework/runtime/` implementation with the
`master` C runtime/capability behavior for the areas called out during review:
`cap_session_mgr`, `cap_skill_mgr`, `cap_llm_config`, `cap_llm_inspect`,
`cap_agent_mgr`, `claw_manager`, `claw_memory`, `claw_skill`, `claw_utils`, and
`claw_core`.

## Overall Conclusion

The new runtime covers many of the same concepts, but it is not behaviorally
equivalent to the `master` C implementation. It is closer to a parallel agent
runtime than a drop-in replacement.

The current app wiring under `components/common/app_claw` still initializes the
old `claw_agent_mgr` and `claw_core` path. Its component manifest also still
depends on the old C modules and capabilities. If the app is later switched to
the Rust runtime ABI, the differences below are expected compatibility risks.

## Session Manager

`master` behavior:

- `cap_session_mgr` exposes `session_command`.
- The user-facing command surface is `/session new`, `/session list`,
  `/session switch`, and `/session delete`.
- Sessions are addressed by user-visible aliases.
- Current session state is scoped by `channel + chat_id`.
- Deleting the current session is refused.
- Delete can invoke the registered session-history cleanup handler.

Current runtime behavior:

- `claw_agent` exposes explicit session operations:
  `claw_agent_session_create`, `open`, `submit`, `interrupt`, `cancel`,
  `receive`, `close`, `delete`, and `list`.
- C ABI session IDs are numeric `uint32_t`; internal Rust IDs are prefixed
  session IDs.
- No equivalent `/session` command layer was found.
- No channel/chat scoped alias map was found.
- Session event streams replace the old command-oriented current-session model.

Risk:

- IM/web users depending on `/session` commands will lose behavior unless a
  compatibility layer maps those commands to runtime sessions and restores
  per-chat current session aliases.

## Skill Manager

`master` behavior:

- `cap_skill_mgr` exposes `list_skill`, `register_skill`, `unregister_skill`,
  and `activate_skill`.
- `register_skill` validates that the file is exactly `<skill_id>/SKILL.md`,
  verifies it exists under the runtime skill root, reloads the registry, and
  confirms the catalog entry matches.
- `unregister_skill` rejects read-only skills, deletes the runtime skill file,
  reloads the registry, and rolls back if reload fails.
- `activate_skill` requires a session id, loads the skill document, persists the
  active skill for that session, and syncs LLM-visible capability groups.
- `activate_skill` returns a `<skill_content ...>` payload.

Current runtime behavior:

- Runtime exposes `skill_list`, `skill_activate`, and `skill_reload`.
- No runtime-native `register_skill` or `unregister_skill` tool was found.
- `activate_skill` returns the processed skill document but does not recreate
  the old persistent per-session active skill state.
- The new registry scans skill roots in priority order and expands
  `{CUR_SKILL_DIR}` when loading documents.

Risk:

- The tool surface is incompatible with `master`.
- Per-session active skills and capability-group visibility do not have the old
  behavior unless a compatibility layer is added.
- Runtime install/uninstall flows need a separate file-management layer plus
  `reload_skills`.

## LLM Config

`master` behavior:

- `cap_llm_config` exposes a `/llm` command surface:
  `status`, `setup`, `token`, `model`, `backend`, `base-url`, and `reset`.
- It supports provider presets such as DeepSeek, OpenAI, Qwen, and Anthropic.
- It stores and applies detailed fields including backend type, base URL, auth
  type, model, timeout, max token field, default image byte limit, tool support,
  vision support, and remote-image-only behavior.
- App wiring can hot-apply config through `claw_agent_mgr_update_core_config`.

Current runtime behavior:

- `claw_agent_init` accepts backend, API key, model, base URL, persistence root,
  and skill roots.
- Backend behavior is owned mostly by `claw-api` backend kinds.
- No equivalent runtime `/llm` command surface was found.
- No public C ABI hot-update equivalent to `claw_core_update_llm_config` was
  found.

Risk:

- Runtime cannot preserve the old dynamic `/llm` setup/status/reset behavior
  without an app-level shim and a runtime config update API.
- Some fine-grained wire options from the C config are no longer caller
  configurable through the runtime ABI.

## LLM Inspect

`master` behavior:

- `cap_llm_inspect` exposes `inspect_image`.
- The tool requires `ctx->core`.
- It calls `claw_core_llm_infer_media()` with a local image path and prompt.

Current runtime behavior:

- `claw-api` has media inference support.
- No runtime-native replacement for `inspect_image` was found.
- The runtime C capability wrapper calls old C capabilities with
  `ClawCapCallContext::default()`.
- The current C `cap_llm_inspect` implementation returns
  `Error: claw_core is not ready` when `ctx->core` is missing.

Risk:

- If `inspect_image` is exposed through the runtime wrapper, it is expected to
  fail unless the wrapper or app layer supplies an equivalent core/media
  context.
- A direct Rust-native `inspect_image` tool backed by `claw-api::infer_media`
  would be needed for behavioral continuity.

## Agent Manager and claw_manager

`master` behavior:

- `cap_agent_mgr` exposes `spawn_agent`, `send_agent_followup`,
  `inspect_agent`, `list_agents`, `close_agent`, and `delete_agent`.
- These tools are root-agent-only and require agent/root-agent caller context.
- `spawn_agent` accepts `prompt`, optional `agent_type`, and optional
  `background`.
- `send_agent_followup` can send additional input to active or closed subagents,
  optionally interrupting.
- `inspect_agent` and `list_agents` report status, phase, last request id, type,
  and last error.
- `close_agent` preserves history; `delete_agent` removes runtime/history state.

Current runtime behavior:

- Runtime internal tools are `subagent_list_spawnable`, `subagent_spawn`,
  `subagent_list`, `subagent_watch`, `subagent_delete`, and
  `conversation_end`.
- `subagent_spawn` accepts `kind`, `name`, `goal`, and optional `termination`.
- Spawn permission is based on agent manifests and graph policy.
- No direct equivalent was found for `send_agent_followup` or `close_agent`.
- The runtime C capability wrapper filters out `ROOT_AGENT_ONLY` C capabilities,
  so old root-agent-only manager tools are not directly exposed as runtime LLM
  tools.

Risk:

- Tool names, schemas, outputs, and lifecycle semantics are incompatible.
- Existing prompts or skills that call old `cap_agent_mgr` tools will not work
  against runtime without a compatibility adapter.

## claw_core

`master` behavior:

- `claw_core` is a C handle with request and response queues.
- Requests carry `request_id`, source/target metadata, session id, flags, and
  user text.
- Callers use `claw_core_submit`, `claw_core_receive`,
  `claw_core_receive_for`, and `claw_core_cancel_request`.
- Context is provided through callback providers.
- Persistence is callback-based through `persist_context`.
- Completion observers, stage messages, request gates, request-start callbacks,
  and agent-loop phase inspection are public behaviors.

Current runtime behavior:

- Runtime uses an orchestrator, session registry, session controls, and session
  event streams.
- C ABI maps stream events to output, reasoning, tools, done, error, and closed.
- Turn/iteration bracket events are skipped by the C ABI event mapping.
- Persistence is checkpoint/coordinator based rather than callback based.
- Interrupt/cancel behavior is driven by session control and runtime
  preemption checkpoints.
- No public `get_agent_loop_phase` equivalent was found.

Risk:

- The request/response queue API and request-id based receive behavior are not
  preserved.
- Existing code that depends on context provider callbacks, completion
  observers, stage notes, or loop phase inspection needs explicit migration.

## claw_memory

`master` behavior:

- `claw_memory` exposes memory capabilities and C integration callbacks.
- Long-term memory tools use JSON-style inputs and outputs.
- It provides session-history persistence, request gating, request-start
  handling, manual-write marking, stage-note collection, and session-history
  deletion hooks.
- Memory item fields are fixed C structs with id, source, content, summary ids,
  tags, keywords, timestamps, access count, and deleted state.

Current runtime behavior:

- Runtime has Rust `ProfileStore`, `LongTermMemory`, `TranscriptStore`, and
  compaction support.
- Runtime tools include `memory_store`, `memory_recall`, `memory_list`,
  `memory_update`, `memory_forget`, `profile_read`, `profile_replace`, and
  `profile_clear`.
- Tool outputs are generally human-readable text rather than the old JSON
  response surface.
- Store behavior includes duplicate detection.
- Transcript and checkpoint storage formats differ from the old C session
  history layout.

Risk:

- Tool output compatibility and persisted data compatibility are not preserved.
- Request gate, stage-note, and manual-write semantics need explicit migration
  if callers rely on them.

## claw_skill

`master` behavior:

- Skill registry supports firmware and runtime skills.
- Runtime skill activation is session-aware.
- Active skill ids can be persisted and restored per session.
- Skill activation can affect LLM-visible capability groups.

Current runtime behavior:

- Skill registry supports multiple roots and root priority.
- DATA skills can shadow system skills when scanned earlier.
- `{CUR_SKILL_DIR}` expansion is supported.
- Activation returns loaded document content, but does not recreate old
  per-session active skill persistence.

Risk:

- Registry behavior is partially aligned, but activation state behavior is not.
- Capability visibility tied to skill activation must be reintroduced above the
  new skill registry if required.

## claw_utils

`master` behavior:

- `claw_utils` provides C utility APIs such as string and time helpers.

Current runtime behavior:

- `claw-utils` is a Rust crate focused on typed IDs, prefixed ID parsing, and
  text truncation helpers.

Risk:

- This is not a drop-in replacement for the old C utility module.
- Any C consumer of the old utility APIs still needs the old module or a C ABI
  compatibility surface.

## Capability Wrapper Notes

The runtime C capability wrapper registers C capabilities as Rust tools only
when they are callable or hybrid, have an execute function, are callable by the
LLM, and are not marked `ROOT_AGENT_ONLY`.

When it invokes a C capability, it currently passes a default empty call
context. Capabilities that depend on `ctx->core`, `ctx->session_id`,
`ctx->channel`, `ctx->chat_id`, or caller identity will not preserve old
behavior through this wrapper unless the wrapper is extended to populate those
fields.

## Additional Project Constraint Risk

The runtime currently exposes and persists `TurnId` / `turn-*` concepts in
`claw-core`, `claw-agent`, tests, and snapshots. Project instructions say to use
`IterationId` only and not introduce legacy `TurnId` aliases or `turn_id`
fields. This should be resolved before treating runtime behavior as aligned
with the current architecture rules.

## Suggested Compatibility Work

To align with `master` behavior, add compatibility shims before switching app
traffic to the runtime:

1. Implement `/session` command compatibility on top of runtime sessions,
   including per-chat alias/current-session mapping.
2. Add old skill tool compatibility for `register_skill`, `unregister_skill`,
   and persistent per-session `activate_skill` behavior.
3. Add `/llm` command compatibility and a runtime config hot-update path.
4. Rewrite or adapt `inspect_image` so it uses runtime media inference without
   requiring old `ctx->core`.
5. Provide old `cap_agent_mgr` tool names and schemas as adapters over the new
   subagent graph, or migrate all prompts/skills to the new tools.
6. Decide whether old `claw_core` request/response APIs remain supported as a
   compatibility layer or are intentionally broken.
7. Define a migration path for old memory/session/skill persisted data.
