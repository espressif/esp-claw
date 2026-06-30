# Agent Layer Architecture Plan

Status: draft (discussion captured, not implemented)

## Goals

- **Agent owns iteration**: `iteration_loop` is called from agent runtime, not orchestrator.
- **Orchestrator stays thin**: spawn/route/push input/collect outcomes; no `ContextAssembler`, `run_iteration`, or phase-specific logic.
- **Pluggable agents**: Frontend / Worker (and future kinds) swap via registry without changing orchestrator.
- **Concurrent agents (future)**: each instance is a `dyn Agent` with `push_input` + `step`; orchestrator does not tick all instances in one loop long-term.
- **Tiered runtime**: light chat ≈ LLM call + context; heavy work → Worker checklist (PLAN/ACT/VERIFY) after Frontend router.

## Concept Stack (terminology)

| Layer | Meaning |
|-------|---------|
| LLM turn (`iteration_loop`) | One LLM HTTP + optional tool batch |
| ReAct micro-loop | Multiple LLM turns within one sub-goal |
| Checklist item (`StepId`) | One verifiable sub-goal on Worker task |
| Agent `step()` | One runtime advance (0 or 1 iteration_loop, or signal-only transition) |
| Worker phase | PLAN (whole checklist once) / ACT+VERIFY (per checklist item) |
| Frontend mode | Conversation vs Task lifecycle (not every message is a task) |

## Two-Trait Design

### `AgentSpec` (stateless, pluggable semantics)

- Phase/mode rules, context fragments, schemas, tool visibility.
- `transition` / `handle_event`.
- Does **not** call LLM or `iteration_loop` (easy to unit test).

### `Agent` (stateful instance — orchestrator only sees this)

```text
push_input(AgentInput)
step(&AgentHost) -> AgentOutcome
```

`step` internally (shared runtime for all kinds):

```text
digest input → should_run_llm? → context assembly → iteration_loop → validate → transition → apply patches locally
```

### `AgentFactory` + `AgentRegistry`

- Register `AgentSpec` + factory per `AgentKind`.
- `register_builtins()` for Frontend/Worker; tests/boards can override.

## Orchestrator Contract (non-invasive)

**Orchestrator may:**

- Create/destroy instances via factory
- `push_input` (user message, protocol event, signal, interrupt)
- Drive `step` (or agent task loop calls `step`)
- Handle structured side effects from `AgentOutcome`
- Inject `AgentHost` (llm, tools, skills, memory, env, policy)

**Orchestrator must not:**

- Call `IterationLoop`, `ContextAssembler`, `parse_and_validate` directly
- Branch on Frontend Intake/Report or Worker PLAN/ACT/VERIFY

## `AgentInput` / `AgentOutcome` (draft)

**Input:**

- `UserMessage { text, route }`
- `ProtocolEvent(AgentEvent)`
- `Signal(TransitionSignal)`
- `Interrupt { messages }`

**Output:**

- `Idle`
- `Replied { text }` — direct user reply (chat or report)
- `Transitioned { events }`
- `Stepped { iteration, transition, trace }`
- `SpawnWorker(TaskCreateParams)` — orchestrator creates worker instance
- `Failed(AgentError)`

Patches applied **inside** agent; orchestrator routes **events** and **spawn** only.

## Frontend Agent (first implementation target)

### Problem

Task phases (Intake → Delegate → Report → Done) must **not** apply to every message (e.g. "你好").

### Dual mode

| Mode | When | Behavior |
|------|------|----------|
| `Conversation` | No active task; casual chat | Chat path: 1× LLM + context; no Delegate/Report |
| `Task(TaskPhase)` | Task intent or active task | FSM: Intake / Clarify / Delegate / Watch / AskApproval / Report / Done |

### Router (Frontend semantic, not orchestrator)

```text
ReplyOnly | StayOnFrontend (light work) | SpawnWorker (heavy task)
```

Heavy tasks → Worker backend agent. Light → Frontend chat/short loop.

### User-facing model

- **One Frontend persona** for the user (supervisor).
- Implementation: Conversation path (cheap) + Task path (FSM) inside same `Agent` instance.

## Worker Agent (later)

- Target: long, complex, governable tasks (checklist, per-item ACT/VERIFY, Paused/Blocked, Progress events).
- Spawned only when Frontend router decides heavy.
- Same `Agent` runtime + `WorkerSpec`; not a separate orchestration path.

## Migration from Current Code

| Current | Target |
|---------|--------|
| `runtime::run_iteration` | `agent::runtime::step` internals |
| `HarnessIterationOutput` / `ActionSummary` | `agent::iteration::IterationStepResult` |
| `InstanceControl` | `agent::control::AgentControl` (per instance) |
| `orchestrator::tick_instance_once` body | `agent.step` + outcome handling |
| `AgentSpec` | Keep, extend with `on_input` / mode |
| `runtime.rs` | Remove after migration |

## Implementation Order

1. Define `AgentKind`, `AgentInput`, `AgentOutcome`, `AgentHost`, `Agent` trait, `AgentFactory`.
2. Implement shared `AgentRuntime` (holds state + `dyn AgentSpec`).
3. Frontend: `FrontendMode`, Conversation vs Task paths, router → `SpawnWorker`.
4. Unit tests: "你好" → `Replied` (no Delegate); task message → Task path or `SpawnWorker`.
5. Wire orchestrator thin adapter (optional later).
6. Worker on same runtime; delete `runtime.rs`.

## Open Questions

- Router: pure LLM vs rules + LLM?
- `AgentKind` vs existing `AgentRole` naming.
- Max iterations per `step` vs agent task inner loop.
- How `AgentContext` (task contract, turn_ctx) is passed per step from orchestrator.

## References

- Layer model: `framework/runtime/claw-core/docs/arch.mermaid`
- Existing `AgentSpec`: `framework/runtime/claw-core/src/agent/spec.rs`
- Iteration semantics: `AGENTS.md` (update `patch`/`reason` wording when implementing)
