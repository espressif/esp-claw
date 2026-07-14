# claw-core Module Refactor Plan

## Why this refactor exists

The current `orchestrator` module owns four different layers at once:

- the public runtime facade;
- process-wide scheduling across sessions;
- the state machine for one session and its turns;
- the agent graph and all multiagent behavior.

At the time this plan was approved, `claw-core/src/orchestrator` contained 46
files and 7,719 lines, more than half of `claw-core/src`. Moving multiagent code
out of `agent` was necessary, but nesting all of it under `orchestrator` did not
establish the final ownership boundaries.

This refactor fixes ownership rather than merely moving files.

## Target top-level modules

`claw-core/src` has seven domain modules:

```text
src/
├── protocol/
├── config/
├── memory/
├── agent/
├── multiagent/
├── session/
├── orchestrator/
└── lib.rs
```

### `protocol`

Owns immutable values shared across runtime layers: ids, `AgentKind`, `Message`,
`TurnOrigin`, `SessionPersistence`, `StreamPart`, and the public
`SessionEvent` protocol. It owns no channels, runtime state, factories, stores,
or schedulers.

It does not depend on another `claw-core` module.

### `config`

Owns runtime configuration: API selection, `ReasoningEffort`, and the generated
agent catalog/manifests. Catalog entries are configuration data, not live agent
or graph ownership.

It may depend on `protocol`.

### `memory`

Owns transcripts, context adapters, profiles, skill context, and long-term
memory integration.

It may depend on `protocol` and `config`.

### `agent`

Owns exactly one agent: its task state machine, command reducer, iteration loop,
tool execution, construction, and private `AgentEvent` output.

It may depend on `protocol`, `config`, and `memory`. It must not import
`multiagent`, `session`, or `orchestrator`.

### `multiagent`

Owns one live graph of agents: topology, scheduling, stable agent slots,
in-flight ticks, approvals at the graph boundary, subagent tools, foreground
completion, background result delivery, and graph persistence.

It may depend on `protocol`, `config`, and `agent`. It must not import `session`
or `orchestrator`.

### `session`

Owns one session: its public control/event halves, turn lifecycle, busy rules,
reasoning effort at turn boundaries, approval conversation, event projection,
one `MultiagentRuntime`, session registry types, and per-session checkpoint
composition.

It may depend on `protocol`, `config`, `agent`, and `multiagent`. It must not
import `orchestrator`.

### `orchestrator`

Owns the public `Orchestrator` facade, its worker, process-wide session lookup,
creation/open/delete/stop commands, global id allocation state, and global
checkpoint coordination. It does not implement turns, agent graph scheduling,
subagent tools, or approval behavior.

It may depend on every lower layer. A direct dependency on `agent` is limited
to constructing the shared agent factory during worker bootstrap.

## Dependency direction

Dependencies only point downward:

```text
orchestrator
    │
    ▼
 session
    │
    ▼
multiagent
    │
    ▼
  agent
    │
    ▼
 memory
    │
    ▼
 config
    │
    ▼
protocol
```

The following imports are forbidden:

```text
agent      -> multiagent / session / orchestrator
multiagent -> session / orchestrator
session    -> orchestrator
```

## Target internal layout

```text
protocol/
├── mod.rs
├── ids.rs
├── message.rs
└── event.rs

config/
├── mod.rs
└── catalog.rs

multiagent/
├── mod.rs
├── runtime.rs
├── drive.rs
├── state.rs
├── agents.rs
├── tool_port.rs
├── tools/
│   ├── mod.rs
│   ├── args.rs
│   ├── spawn.rs
│   ├── list_spawnable.rs
│   ├── list.rs
│   ├── watch.rs
│   ├── followup.rs
│   └── delete.rs
└── persistence/
    ├── mod.rs
    ├── codec.rs
    ├── schema.rs
    └── error.rs

session/
├── mod.rs
├── api.rs
├── actor.rs
├── state.rs
├── approval.rs
├── registry.rs
└── persistence.rs

orchestrator/
├── mod.rs
├── handle.rs
├── engine.rs
└── checkpoint.rs
```

File boundaries may be adjusted when a cohesive file would otherwise exceed
the repository's 1,500-line guideline. Tiny one-struct forwarding files are
not retained merely to match this sketch.

## State ownership changes

### Remove `OrchestratorInstance`

The type becomes top-level `MultiagentRuntime`. The complete
`orchestrator/instance` subtree is removed; no compatibility alias remains.

### Remove forwarding state layers

`OrchestratorInstanceState`, `GraphState`, and `SchedulerState` become one
`MultiagentState` that directly owns topology, the ready queue, and parked
approvals. Cross-state mutations such as subtree deletion remain atomic methods
on this owner.

### Finish the `AgentSlot` ownership model

Each graph node has one stable slot containing both its inbox and execution
state:

```rust
struct AgentSlot {
    inbox: VecDeque<Message>,
    execution: AgentExecution,
}

enum AgentExecution {
    Idle(BaseAgent),
    Running {
        future: AgentTickFuture,
        abort: AgentAbortHandle,
    },
}
```

This removes the cross-table invariant between `AgentSlots` and
`InflightAgentTasks`. Deleting a child removes its slot only after foreground
waiters and pending results have been resolved.

### Replace session wrappers with one owner

`SessionRuntime`, `SessionDrive`, `InstanceSlot`, and Engine-owned turn methods
become one long-lived `SessionActor`. The actor owns the turn state, event sink,
control acknowledgements, and `MultiagentRuntime`.

`SessionControl` sends directly to the actor's command channel after a session
is opened. The Engine polls session actors cooperatively on the existing single
worker thread; this does not create a thread per session.

## Runtime flow

```text
SessionControl
      │ SessionCommand
      ▼
SessionActor
      ├── SessionState / active Turn
      ├── event projection
      ├── approval conversation
      └── MultiagentRuntime
              └── AgentSlot[*]
```

### Foreground spawn

`subagent_spawn(foreground: true)` creates a child and a one-shot completion.
The root tool future remains pending while `MultiagentRuntime` drives the child.
The child result completes that tool future and stays inside the current turn.

### Background spawn

`subagent_spawn(foreground: false)` returns the child id immediately. The child
continues while the session actor remains able to process control commands and,
when no root turn is active, another user submission.

The completed child result is appended to its parent's `AgentSlot` inbox. A
result reaching the root opens a `TurnOrigin::Subagent` turn only at a legal
turn boundary. If a user turn is active, the result remains queued until that
turn ends; root turns never overlap.

### Followup

Followup remains live-only. A running target is interrupted at a safe boundary,
then the followup `Message` is delivered to the same in-memory agent. Completed
agents are not reconstructed from transcripts.

## Implemented result

The refactor described above is now the source layout, not a compatibility
facade over the old tree:

- `OrchestratorInstance`, `SessionRuntime`, `SessionDrive`, `InstanceSlot`,
  `GraphState`, `SchedulerState`, and `InflightAgentTasks` were removed;
- `SessionControl` now sends directly to a leased session-actor command channel;
- `orchestrator` contains only `mod.rs`, `handle.rs`, `engine.rs`, and
  `checkpoint.rs`;
- multiagent tools use one caller-bound `tool_port`; `tools/mod.rs` assembles the
  group, shared invocation parsing lives in `tools/args.rs`, and each model-facing
  tool owns one implementation file;
- checkpoint parts are now `session-state`, `multiagent-runtime`, and
  `agent-id-allocator`; the removed checkpoint layout is intentionally rejected;
- checkpoints requested while agent slots are running are deferred until a
  checkpoint-safe slot boundary.

The forbidden dependency audit is clean, and the session API plus foreground,
background, cancellation, and concurrent-user/subagent lifecycle matrices pass.

## Event boundary

The single-agent runtime emits private `AgentEvent` values. The session layer
maps root events into the public `SessionEvent` stream and adds
`TurnStarted`/`TurnEnded`. Subagent events use a disabled observer and never
leak to the public session stream.

The existing `StreamPart<Delta/End>` ordering remains unchanged.

## Checkpoint boundary

One session checkpoint batch atomically contains:

```text
SessionState
+ MultiagentState
+ AgentSlot[*]
```

The process-wide durable state contains the session registry and global agent
id allocator. Runtime channels, wakers, futures, event sinks, and foreground
receivers remain process-local.

This refactor does not retain old internal module aliases, duplicate state
owners, or dual checkpoint decoders. Changed durable layouts receive a new
schema version.

## Verification policy

The existing session and subagent behavior matrices are the regression
contract. Directory moves, getters, and forwarding removal do not receive
artificial unit tests. A new integration case is added only when an externally
observable foreground/background, turn, control, or persistence behavior is
not already covered.

The final verification runs formatting, `cargo check`, and the affected
`claw-core`/`claw-agent` session, subagent, and checkpoint tests.

## Not in scope

- No manual child lifetime mode; children remain auto-lifecycle agents.
- No transcript-based resume for completed subagents.
- No parallel root turns and no OS thread per session or subagent.
- No change to the public foreground/background or session event semantics.
- No compatibility facade for the old private module layout.

## Implementation sequence

1. Establish `protocol` and `config` leaf ownership and remove the current
   `agent`/`session` type cycle.
2. Extract and consolidate `multiagent`, including state, slots, tool port, and
   persistence.
3. Move per-session behavior into `SessionActor` and route `SessionControl`
   directly to it.
4. Reduce `orchestrator` to its process-wide responsibilities and delete the
   old engine/instance subtrees.
5. Update architecture and checkpoint documentation and run the behavior
   regression suite.
