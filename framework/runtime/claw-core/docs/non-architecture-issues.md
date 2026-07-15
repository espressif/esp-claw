# claw-core Non-Architecture Issue Register

This register captures behavior, contract, documentation, test, CI, and
dependency issues found before the architecture-boundary refactor. It is a
handoff list, not a target architecture. Unless marked otherwise, every item is
open.

The evidence paths below describe the pre-refactor tree as inspected on
2026-07-14. Crate-local paths are relative to `framework/runtime/claw-core`;
other paths are repository-relative. Files may move during the refactor; an
issue is closed only when its exit condition is satisfied, not when its old
evidence path disappears.

## Scope

Included here:

- externally observable behavior and configuration-contract mismatches;
- agent manifest and tool/skill schema semantics;
- public API snapshot and CI reproducibility;
- failing or missing behavioral tests;
- stale documentation and source comments;
- build-only code or unused dependencies left on runtime manifests.

Explicitly excluded because they belong to the architecture-boundary refactor:

- ownership of session, drive, graph, scheduler, approval, and agent state;
- replacement of correlated flags with explicit state machines;
- `Engine`, `OrchestratorInstance`, `Agent`, `GenericAgent`, and `BaseAgent`
  boundaries;
- dependency-injection and composition-root design;
- module/file consolidation and internal visibility cleanup.

Priorities in this document mean:

- **P0**: ambiguous policy or a currently failing required test;
- **P1**: incorrect contract, persisted-data ambiguity, or missing release gate;
- **P2**: documentation, maintenance, or dependency hygiene debt.

## Summary

| ID | Priority | Area | Problem |
| --- | --- | --- | --- |
| `NAR-001` | P0 | Tools | `tool_groups` is described as an allowlist but omitted hidden groups remain discoverable and loadable. |
| `NAR-002` | P1 | Skills | **Resolved:** per-kind skill baking was removed; every agent receives the AgentSystem's shared skill catalog. |
| `NAR-003` | P2 | Spawn policy | The `allowed_kinds` comment says the policy is not enforced although runtime code enforces it. |
| `NAR-004` | P1 | Memory | “Per-agent” long-term memory is physically scoped by agent kind, so same-kind agents share a store. |
| `NAR-005` | P1 | Checkpoints | **Resolved during the boundary refactor:** the unused second construction path was removed. |
| `NAR-006` | P1 | Public API | The committed `claw-core` API snapshot does not describe the current API. |
| `NAR-007` | P1 | CI | Public API checks are not reproducible from the checked-in toolchain and are not run by a checked-in workflow. |
| `NAR-008` | P0 | Tests | The adjacent `claw-agent` integration baseline is not green; Cargo initially exposes only the first failing binary. |
| `NAR-009` | P1 | Tests | The compound tool-policy contract has no behavioral coverage. |
| `NAR-010` | P2 | Documentation | Several source comments and repository guides contradict current Rust behavior. |
| `NAR-011` | P2 | Dependencies | Build-only tool baking is exposed by the runtime `claw-tool` crate and leaves manifest debris. |
| `NAR-012` | P1 | Manifests | **Resolved:** decorative `schema_version` fields were removed from baked manifests. |
| `NAR-013` | P1 | Manifests | A missing `tool_block_retries` field silently changes policy to zero retries. |
| `NAR-014` | P1 | Permissions | **Resolved:** every session exposes a durable, live `Deny` / `Ask` / `AllowAll` permission level. |
| `NAR-015` | P1 | Checkpoints | Several checkpoint decoders ignore schema versions or silently normalize invalid state. |
| `NAR-016` | P0 | Subagents | **Resolved:** each live agent now has a stable slot whose inbox survives while the agent is in flight. |
| `NAR-017` | P0 | Concurrency | A shared async LLM lease keeps only one waiter waker, so concurrent extraction can stall indefinitely. |
| `NAR-018` | P0 | Tests | Control-progress tests race their workers and can leave the test process hung after failure. |
| `NAR-019` | P0 | Tests | The backend retry matrix expects six transient HTTP calls but the runtime makes four. |
| `NAR-020` | P0 | Tests | The built-in subagent fixture expects an obsolete unknown-kind error fragment. |
| `NAR-021` | P0 | Tests | The tool-registry fixture expects an obsolete duplicate-tool error fragment. |
| `NAR-022` | P0 | Persistence | Tool-registry checkpoint timing/state assertions disagree with persisted output. |
| `NAR-023` | P1 | Tracing | Streaming iteration chat spans do not contain the attempt span required by the trace test. |
| `NAR-026` | P1 | Tracing | **Resolved in Rust:** overlapping session/agent futures now use distinct logical tasks and independently seeded context. |
| `NAR-027` | P1 | Tracing | **Resolved:** only explicit `counter.<series>` fields create Chrome counter tracks. |

## Behavior and Configuration Contracts

### NAR-001: `tool_groups` does not currently form a strict allowlist

`AgentManifest::tool_groups` and the factory comments describe the field as the
set of registry groups an agent may use. The implementation calls
`ToolSet::retain_registry_groups`, which changes omitted registry tools to
`Disabled` rather than removing or permanently denying them.

Capability groups registered by `claw-cabi` use `default_visibility = false`.
A disabled tool with that visibility is added to the discovery catalog, and
`tool_load` can enable it durably. Therefore an omitted capability group may be
found with `tool_search` and enabled after the manifest projection is applied.

Current evidence:

- `src/agent/manifest.rs` (`AgentManifest::tool_groups`);
- `src/agent/factory/create.rs` (`retain_registry_groups` call);
- `framework/runtime/claw-tool/src/set.rs` (`retain_registry_groups`, `is_loadable`,
  `refresh_discovery_catalog`, and `apply_pending_tool_loads`);
- `framework/runtime/claw-cabi/src/tool.rs`
  (`ToolGroup::new(group_id, false, tools)`).

This may be either a policy bypass or an inaccurately named lazy-loading
contract. It must be decided explicitly.

Exit condition:

- If the field is an allowlist, omitted groups cannot be searched, loaded, or
  called, and a test proves all three properties.
- If the field is an initial-visibility list, rename/re-document the field and
  add a test proving the intended discovery/load behavior.

### NAR-002: manifest skill ids are catalog-only

**Status: resolved.**

The build-time schema says `skills/skills.json` lists the skill ids the kind
loads. Parsing, inheritance, validation, and code generation preserve those
ids. At runtime, `FsAgentFactory::resolve_config` only emits the
`manifest_ids_catalog_only` trace event and supplies the complete shared
`SkillSet` to every agent.

All checked-in skill lists are currently empty, which masks the mismatch.

Current evidence:

- `manifest_gen/model.rs` (`SkillsJson`);
- `manifest_gen/agent_manifests.rs` (common/kind merge);
- `src/agent/manifest.rs` (`AgentManifest::skills`);
- `src/agent/factory/create.rs` (`resolve_config`).

Resolution: skills are AgentSystem-level runtime data, not agent-kind manifest
data. The `skills/skills.json` resources, parser and code-generation fields,
runtime manifest field, and catalog-only trace event were removed. Agent kinds
still select instructions, tool groups, and spawn policy; every agent receives
the shared filesystem-backed `SkillSet` assembled from `skill_roots`.

### NAR-003: `allowed_kinds` documentation is stale

`AgentManifest::allowed_kinds` says the policy is “not yet enforced at
runtime.” `SpawnSubagentTool` rejects disallowed kinds before emitting a graph
effect, and `subagent_list_spawnable` is built from the same policy.

Current evidence:

- `src/agent/manifest.rs` (`allowed_kinds` field comment);
- `src/agent/tools/spawn_subagent.rs` (`SpawnPolicy::allows` check).

Exit condition: update the field and manifest-generator documentation to state
the actual enforcement point, then keep an allowed/disallowed spawn test.

### NAR-004: long-term memory scope is named inconsistently

The memory adapter and `claw-memory` documentation describe an agent-private
store. The factory path is `<long_term>/agents/<kind>`, not a path containing
the agent id. Multiple instances of the same kind therefore share that store.
Some factory comments call this “per-agent-kind,” while other comments and
errors still call it “per-agent.”

Current evidence:

- `src/agent/factory/long_term.rs` (`agent_root_dir` layout);
- `src/agent/factory/create.rs` (`join_storage_path(..., kind.as_str())`);
- `src/memory/long_term_memory_adapter/mod.rs` (agent-private description);
- `framework/runtime/claw-memory/README.md` (per-agent description).

Exit condition: make an explicit product decision between per-instance and
per-kind memory, use one term everywhere, and add a two-agent isolation/sharing
test for the chosen behavior. If the on-disk layout changes, document migration
or deliberate reset behavior.

### NAR-005: checkpoint defaults drift between constructors

**Status: resolved during the architecture-boundary refactor.**

The direct `claw-core::Orchestrator::new` path uses checkpoint interval `1`,
while the product `claw-agent::AgentSystem` composition uses interval `30`.
Both use the same directory name and history count. A caller's persistence
frequency therefore depends on which public construction path it selects.

Current evidence:

- `src/orchestrator/mod.rs` (`CHECKPOINT_INTERVAL = 1`);
- `framework/runtime/claw-agent/src/lib.rs` (`CHECKPOINT_INTERVAL = 30`).

Resolution: the unused direct `Orchestrator::new` construction path and its
private interval constant were removed. `AgentSystem` is now the only product
composition path that chooses the interval; the lower-level constructor accepts
an already-configured checkpoint coordinator and introduces no second default.

### NAR-012: manifest versions are decorative

**Status: resolved.**

`agent.json` and `tools/tools.json` require a `schema_version`, but the
build-time serde shapes suppress the resulting dead field warning and never
validate the value. A file declaring an unknown version is therefore accepted
and interpreted as the current shape. The version cannot provide compatibility
or reject unsupported data in its present form.

Current evidence:

- `manifest_gen/model.rs` (`AgentJson` and `ToolsJson`);
- `manifest_gen/parse.rs` (no version validation);
- `resources/agents/**/{agent.json,tools/tools.json}`.

Resolution: the unused fields were removed from `AgentJson`, `ToolsJson`, and
all checked-in agent manifest resources. These files are compiled together with
their parser and have no independent runtime or persistence lifecycle, so build
failure is the format compatibility boundary.

### NAR-013: missing tool-block policy silently falls back to zero

`RuntimeJson::tool_block_retries` uses `#[serde(default)]`. Omitting the field
therefore produces a valid manifest with a stricter zero-retry policy, even
though all current manifests specify the value explicitly. A typo or incomplete
manifest changes behavior instead of failing the build.

Current evidence:

- `manifest_gen/model.rs` (`RuntimeJson::tool_block_retries`);
- `src/agent/manifest.rs` (documents the zero fallback);
- both checked-in `resources/agents/*/agent.json` files specify the field.

Exit condition: make the field required and add a missing-field failure test,
or explicitly define it as optional product behavior with a schema-backed
compatibility reason.

### NAR-014: product assembly always allows tool calls

**Status: resolved.**

`PermissionLevel::{Deny, Ask, AllowAll}` is now an explicit session setting.
`SessionState` owns and persists the selected level, while `SessionActor` keeps
a shared live policy synchronized so changes affect the next action
authorization even during an active turn. The same policy is injected into the
root and every subagent; `BaseAgent` remains unaware of sessions and consumes it
only through `PermissionPolicy`.

`Deny` rejects side-effecting actions, `Ask` reaches the existing human approval
and grant flow, and `AllowAll` preserves the previous product behavior. Safe
actions remain available at every level. Public integration coverage verifies
live switching, the complete Ask/approve flow, and isolation between sessions
in `claw-agent/tests/agent_loop_matrix.rs`. Approval is exposed as a typed
same-turn `InputRequested` event and resumed with `respond`; caller adapters own
how that request is presented.

### NAR-015: checkpoint validation is inconsistent

Some durable parts validate their declared schema before decoding, while others
decode the bytes regardless of `PartStateSlice::schema_version`. The session
registry also sorts, de-duplicates, and advances its id counter during decode,
turning malformed state into a different valid state without reporting the
corruption.

Current evidence:

- `src/orchestrator/engine/session_drive.rs` and
  `src/agent/base_agent/persistence.rs` reject unknown schemas;
- `src/orchestrator/engine/persistence.rs`, `src/session/mod.rs`, and
  `src/orchestrator/instance/persistence/codec.rs` do not consistently reject
  them;
- `SessionStoreState::normalize` repairs duplicate/out-of-order session data.

Exit condition: every durable part explicitly accepts only supported schemas,
and invalid registry/graph invariants fail restore or use a documented versioned
migration. Add unknown-schema and corrupt-state tests for each decoder.

### NAR-016: queued subagent result is deleted with its child (resolved)

The registry now keeps one stable `AgentSlot` per live graph node. A slot owns
the node's inbox even while its `BaseAgent` is checked out for a tick. Child
results are converted to parent messages at delivery time and stored only in
the parent's slot, so deleting the child cannot delete an already delivered
result. The lifecycle matrix forces the parent to remain in flight until the
auto-terminated child finishes and verifies that the parent still receives the
result.

### NAR-017: shared async LLM lease can lose waiters

`SharedAsyncLlmState` stores only one `Option<Waker>`. The shared
`LlmExtractor` can be awaited by several agents concurrently, so each later
pending poll replaces the previous waiter's waker. Returning the lease wakes
only the last waiter; an earlier waiter can remain asleep indefinitely even
though the lease becomes available.

Current evidence:

- `src/memory/async_llm.rs` owns the single waker slot (moved from
  `long_term_memory_adapter/async_llm.rs` during the boundary refactor);
- `LongTermDeps` shares one extractor across all agents created by a factory.

Exit condition: use a waiter-aware lease primitive or maintain and wake a
deduplicated waiter set, then cover at least two concurrent extractor waiters
with a progress test.

### NAR-018: control-progress tests are racy and can hang after failure

`pending_request_control_ends_the_turn_before_returning` waits for worker
progress with only 10,000 calls to `thread::yield_now()`. The worker can
legitimately start later, causing a nondeterministic `request did not become
pending` panic. That failure path leaves the agent worker alive, so the test
process may remain running indefinitely instead of reporting the failed test.

`turn_control_preserves_agents_on_interrupt_and_deletes_them_on_cancel` uses
the same 10,000-yield pattern and likewise fails nondeterministically with
`worker did not enter its pending task`, followed by a stuck test process.

This was reproduced both on the pre-refactor `HEAD` worktree and on the
refactored tree. Depending on scheduling, the cancel case completes and the
interrupt case races, or the first case races immediately.

Current evidence:

- `framework/runtime/claw-agent/tests/async_tool_control_matrix.rs`
  (`wait_for`, the pending-request test, and its failure cleanup path);
- `framework/runtime/claw-agent/tests/subagent_lifecycle_matrix.rs`
  (`wait_until_control_worker_is_pending`).

Exit condition: replace scheduler-spin counting with a bounded real progress
wait, guarantee worker teardown on every assertion/failure path, and prove the
test terminates reliably under repeated execution.

### NAR-019: transient-exhaustion retry fixture disagrees with runtime behavior

The `http-transient-exhausts-retries` row expects six HTTP calls and four timer
sleeps, but the matrix observes four HTTP calls before reaching its assertion.
The same 4-versus-6 failure reproduces on the pre-refactor `HEAD` worktree and
on the refactored tree.

Current evidence:

- `framework/runtime/claw-agent/tests/backend_failure_matrix.rs`;
- `framework/runtime/claw-agent/tests/fixtures/backend_failure_matrix.csv`.

Exit condition: decide which operations and retry budgets the count is intended
to cover, then align the fixture or runtime contract and keep an assertion that
distinguishes permanent errors, recovery, and exhausted transient retries.

### NAR-020: built-in subagent validation fixture expects stale wording

The `builtin_subagent_validation` fixture requires the error fragment `not a
known agent kind`. The current spawn-policy boundary rejects `ghost` earlier as
`not permitted for this agent` and lists the allowed kind. Consequently the
behavior is an error as intended, but the exact fixture contract fails. The
same failure reproduces on the pre-refactor `HEAD` worktree.

Current evidence:

- `framework/runtime/claw-agent/tests/builtin_tool_matrix.rs`;
- `framework/runtime/claw-agent/tests/fixtures/builtin_tool_cases.csv`.

Exit condition: decide whether policy denial or catalog lookup owns this case,
then assert the selected stable error contract without depending on wording
owned by the other boundary.

### NAR-021: duplicate-registration fixture expects stale error ownership

The `duplicate-register` data-driven case expects `tool already exists: alpha`,
while registration rejects the duplicate group as `tool group already exists:
alpha`. The same failure reproduces on the pre-refactor `HEAD` worktree.

Current evidence:

- `framework/runtime/claw-agent/tests/data_driven_api.rs`;
- its tool-registry mutation fixture.

Exit condition: decide whether duplicate group or duplicate tool identity owns
the case and make the fixture assert that boundary's stable error.

### NAR-022: tool-registry checkpoint expectations disagree with persisted state

Three persistence tests fail identically on pre-refactor `HEAD` and the
refactored tree:

- stopping all tools leaves the latest persisted started flag as `true`, not
  the expected `false`;
- disabling a directly registered tool leaves its persisted enabled flag as
  `true`, not the expected `false`;
- after 54 registrations the latest checkpoint step is `2`, not `54`.

Current evidence:

- `framework/runtime/claw-agent/tests/persistence.rs`
  (`tool_registry_start_state_writes_checkpoint`,
  `tool_registry_direct_mutations_checkpoint_and_restore`, and
  `tool_registry_keeps_only_two_checkpoints_across_fifty_four_registrations`).

Exit condition: define whether every direct registry mutation must publish an
immediate checkpoint or follows coordinator cadence, then align hooks and tests
and prove restored start/enabled state matches the last acknowledged mutation.

### NAR-023: streaming iteration trace lacks an attempt child span

`iteration_preparation_traces_auxiliary_llm_work_without_payloads` finds
`api.attempt` children below extraction and compaction chat spans, but not below
the streaming user-iteration `api.chat` span. The same failure reproduces on
pre-refactor `HEAD`.

Current evidence:

- `framework/runtime/claw-agent/tests/runtime_trace.rs`;
- the `claw-api` streaming chat path used by `IterationLoop`.

Exit condition: make streaming and non-streaming retry attempts obey the same
structural trace contract, or explicitly narrow the test if streaming attempts
are intentionally represented elsewhere.

### NAR-026: async sibling spans corrupt reconstructed context

**Status: resolved in Rust; Python tree reconstruction remains unchanged because
each independently scheduled Rust future now supplies a valid logical lane.**

`FlatTreeSubscriber` emits one `enter`/`exit` pair for each span's
creation/destruction lifetime, not for each poll of an instrumented future. It
also records an explicit `parent` edge for every span. The offline tree builder
nevertheless reconstructs incremental context by replaying those lifetime
records as a per-`task` LIFO stack.

Concurrent sibling futures commonly share one executor thread, overlap in time,
and finish out of LIFO order. Their lifetime records therefore do not form a
stack. A later sibling can become the parser's apparent ancestor even when the
recorded `parent` points to an earlier sibling. In the 2026-07-14 simulator
capture, span 75 has `parent=67` (the `agent-2` span) but is exported with
`agent-1`; parent-edge reconstruction disagrees with the current parser for 21
of 137 spans and 10 of 94 events.

Current evidence:

- `framework/runtime/claw-log/docs/trace-format.md` defines lifetime-level
  `enter`/`exit` records but also requires stack-based context reconstruction;
- `framework/runtime/claw-log/scripts/claw_trace/tree.py` derives an entering
  span's ancestor from the top of a per-task stack and unwinds that stack on
  exit;
- `framework/runtime/claw-log/scripts/tests/test_tree.py` covers only a strictly
  nested, LIFO trace fixture;
- `framework/runtime/claw-agent/simulator.log` contains the overlapping sibling
  agent spans and the concrete parent edges described above.

Resolution:

- every long-lived session actor future opens its root span with
  `trace.task=<session-id>`;
- every in-flight `claw-core` agent future opens its root span with
  `trace.task=<agent-id>`;
- `FlatTreeSubscriber` consumes that reserved Rust span field, stores the
  logical task on the span, and makes descendants, events, and the lifetime
  `exit` inherit it instead of reading the current executor thread;
- a logical-task root repeats its complete effective grouped context, so the
  existing per-task offline stack starts with `system + session` for an actor or
  `system + session + turn + agent` for an agent and does not depend on another
  task's stack;
- `AgentSlots` allows only one in-flight future for an agent id, so concurrently
  live agent siblings cannot share a task label. Any future independently
  scheduled below an agent must likewise open a distinct `trace.task`, as now
  required by the trace-format contract.

`framework/runtime/claw-log/tests/trace.rs` covers two overlapping logical
agent tasks on one physical thread, full context seeding, descendant/event task
inheritance, and close-time task stability across physical threads.
`framework/runtime/claw-agent/tests/logical_task_trace.rs` verifies the product
session and agent logical-task roots, including that the agent root seeds
`system + session + turn + agent`. The same integration test also covers the
agent-system root, system-scoped startup, and restored-session attribution. The
format and `claw-core` trace vocabulary document the logical-task rule. The
Python Chrome exporter maps complete session scopes, system-only scopes, and
records outside both scopes to separate Chrome processes. It rejects the legacy
session-without-system shape and does not reconstruct async context by guessing
from physical threads.

### NAR-027: numeric event fields create spurious Chrome counter tracks

**Status: resolved.**

The exporter previously parsed every numeric-looking event `key=value` field
and emitted an additional `ph=C` record. That turned lifecycle attributes such
as `argument_bytes`, `output_bytes`, `replace_count`, and one-off `count` fields
into unrelated Chrome/Perfetto counter tracks.

Counter generation is now explicit:

- only `counter.<series>=<number>` opts a field into `ph=C`, with the prefix
  removed from the exported series name;
- every ordinary numeric field remains only in the instant event's `args`;
- a nonnumeric explicitly marked counter fails export rather than being guessed
  or silently ignored;
- negative lifecycle coverage and positive explicit RAM/gauge coverage live in
  `framework/runtime/claw-log/scripts/tests/test_chrome.py`.

Regenerating `framework/runtime/trace.json` from the current simulator log
produces zero `ph=C` records. The `arguments`, `completed`, `submit_accepted`,
`subtree_deleted`, `tool_calls`, and `tool_round_completed` instant events and
their numeric arguments remain present.

## Public API and CI

### NAR-006: the committed public API snapshot is stale

`framework/runtime/snapshots/claw-core.txt` still contains `TurnCause`, a zero-argument
`Orchestrator::session_create`, and `SessionControl::submit(Into<String>)`.
Current code instead exposes `SessionPersistence`, accepts `Message`, exposes
reasoning-effort control, includes close-persistence errors, and emits the
current `ToolCall` event shape.

Exit condition: after the architecture refactor settles, regenerate the
snapshot with `framework/runtime/update-public-api-snapshots.sh`, review the
diff as an API change, and make `framework/runtime/check.sh` pass.

### NAR-007: the public API gate is not reproducible or continuously enforced

`framework/runtime/check.sh` and
`framework/runtime/update-public-api-snapshots.sh` require `cargo-public-api`,
but the checked-in `framework/runtime/rust-toolchain.toml` pins only `stable`;
the audit environment could not generate the snapshot because the required
nightly rustdoc toolchain/target was unavailable. The scripts' install message
mentions only installing `cargo-public-api`.

No checked-in GitHub workflow invokes `framework/runtime/check.sh`; the only
workflow currently present is the approved-PR synchronization workflow.

Exit condition:

- pin or bootstrap the exact toolchain/target and `cargo-public-api` version;
- document one clean-environment command that reproduces the snapshots;
- run the check in the repository's required CI path.

## Test Baseline and Missing Coverage

### NAR-008: `claw-agent` integration tests are not green

Observed before refactoring:

```text
cargo test -p claw-core
  PASS: 10 unit tests, 5 integration tests

cargo test -p claw-agent --tests
  FAIL: agent_loop_csv_tool_matrix_runs_tools_and_feeds_results_to_next_iteration
        case started_enabled_tool_success: expected 1 invocation, observed 0
  FAIL: agent_loop_csv_llm_response_matrix_reports_errors_and_bounds_reasoning
        case plain_with_long_reasoning_truncates: expected 2003 bytes, observed 2000
```

Cargo stops after the failing `agent_loop_matrix` binary, so this initial run
did not execute the later integration binaries. Targeted clean-`HEAD` runs
subsequently reproduced the additional failures and hangs recorded in
`NAR-018` through `NAR-023`; they are baseline issues, not refactor regressions.

The first failure may indicate changed tool-visibility behavior or a stale
fixture. The second may indicate that the fixture still expects a truncation
suffix while the configured `reasoning_short` contract is now a strict 2000
bytes. They remain untriaged here; this register does not guess which side is
correct.

Exit condition: decide the intended contracts, update implementation or
fixtures accordingly, and make `cargo test -p claw-agent --tests` pass without
weakening assertions.

### NAR-024: the full `claw-core` clippy gate is blocked by `claw-api`

`cargo clippy -p claw-core --tests -- -D warnings` fails while compiling the
unchanged `claw-api` dependency. `backends/sse.rs` currently violates its own
`arithmetic_side_effects` and `indexing_slicing` deny lints at lines 102, 103,
190, 364, and 383. The scoped command
`cargo clippy -p claw-core --tests --no-deps -- -D warnings` passes, so this is
recorded rather than mixed into the architecture refactor.

Exit condition: make `claw-api` pass its declared clippy policy, then restore
the full dependency-inclusive command as the `claw-core` lint gate.

### NAR-025: `claw-memory` long-term-memory tests call removed free functions

`cargo test -p claw-memory` does not compile
`tests/long_term_memory.rs`. The tests construct a `LongTermMemory` instance but
still call removed free functions such as `memory_store`, `memory_recall`,
`memory_list`, `memory_update`, and `memory_forget`; 19 unresolved-name errors
are reported. `claw-memory` is unchanged by this refactor.

Exit condition: route those assertions through the constructed
`LongTermMemory` instance and make the crate test suite compile and pass.

### NAR-009: cross-feature contracts lack behavioral tests

Existing tests cover individual tool-set operations, but the audit found no
test combining manifest projection, a hidden registry group, discovery, and
`tool_load`.

Exit condition: add black-box tests for the final decision in `NAR-001`. The
tests should use product-visible behavior rather than opening new internal APIs
solely for test access.

## Documentation and Comment Drift

### NAR-010: comments and guides contradict the implementation

Known remaining examples:

- `src/agent/manifest.rs` says `allowed_kinds` is not enforced; the spawn tool
  enforces it.
- `src/agent/generic_agent.rs` says HTTP/timer transports are injected; the
  construction path currently creates them through `Default`.
- long-term-memory comments alternate between per-agent and per-kind semantics
  (`NAR-004`).
- the repository-level agent guidance and `.agents/design.md` primarily route
  readers to the older C `components/claw_modules/claw_core` implementation and
  do not describe the Rust runtime as a separate active implementation.

The overall Rust comment ratio was about 14%, but several facade/abstraction
files were between roughly 50% and 68%. The problem is not the aggregate amount;
it is duplicated narrative that has become a second, stale specification.

Exit condition: after code movement settles, remove contradicted narration,
keep comments that explain durable invariants or non-obvious contracts, and
update the repository routing docs to distinguish the C and Rust runtimes.

## Dependency and Build Hygiene

### NAR-011: build-only baking code leaks into runtime dependencies

`claw-tool` unconditionally exposes `pub mod bake`, whose filesystem validator
uses `anyhow`. The only production-tree caller found is the `claw-core` build
script, yet `anyhow` is a normal `claw-tool` dependency rather than a build-only
dependency or feature-gated host dependency. This makes build-time tooling part
of the runtime crate's public API and compilation surface.

Separately, `claw-core` declares `dotenvy` as a dev-dependency without any use
inside the crate, and repeats `serde_json` under dev-dependencies even though it
is already a normal dependency.

Current evidence:

- `framework/runtime/claw-tool/src/lib.rs` and
  `framework/runtime/claw-tool/src/bake.rs`;
- `framework/runtime/claw-tool/Cargo.toml` (`anyhow`);
- `manifest_gen/main.rs` (the only `claw_tool::bake` caller found);
- `Cargo.toml` (`dotenvy` and duplicate `serde_json` dev entries).

Exit condition: move or feature-gate the baking validator so it is not in the
runtime API, move `anyhow` to the resulting host/build-only dependency surface,
remove unused/redundant `claw-core` dev dependencies, and run the workspace
build/tests for host and firmware targets.

## Related Register

Compatibility differences between the Rust runtime and the existing C runtime
are tracked separately in `../../behavior_divergence.md`. They should not be
silently reclassified as architecture-boundary work.
