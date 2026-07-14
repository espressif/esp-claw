# claw-core Architecture

## Request path

```text
AgentSystem
  -> Orchestrator
  -> SessionActor
  -> MultiagentRuntime
  -> BaseAgent
  -> IterationLoop
  -> claw-api / claw-tool
```

`Orchestrator` owns one worker thread. The worker cooperatively polls independent
`SessionActor` futures; it does not run one thread per session. Once a session is
opened, `SessionControl` sends directly to that actor rather than routing every
turn command back through the process-wide engine.

## Module boundaries

Dependencies point in one direction:

```text
orchestrator -> session -> multiagent -> agent -> memory -> config -> protocol
```

The important negative boundaries are:

- `agent` owns exactly one agent and knows nothing about graphs, sessions, or orchestration;
- `multiagent` owns one graph and imports neither `session` nor `orchestrator`;
- `session` owns turns and public session I/O and imports no orchestrator code;
- `orchestrator` is the process shell, not a second home for turn or graph behavior.

## Runtime owners

| Owner | Responsibility | Durable state |
| --- | --- | --- |
| `Orchestrator` | Public facade, worker lifetime, session registry, open/delete/stop routing | session registry and process-wide agent-id allocator |
| `SessionActor` | One session's command stream, turn boundaries, busy rules, approvals, event projection, and checkpoint boundary | `SessionState` plus `MultiagentRuntime` |
| `MultiagentRuntime` | One root/child graph, scheduling, result delivery, spawn/followup/delete, and graph approvals | `MultiagentState` and all stable `AgentSlot`s |
| `AgentSlot` | One live graph node's inbox and execution state | inbox and the idle agent's durable parts |
| `BaseAgent` | One agent task, command reduction, transcript context, and iteration execution | canonical task state and tool state |
| `IterationLoop` | One LLM request and its optional tool round | none; it borrows iteration inputs |

`AgentSlot` is the only owner of an agent after construction:

```text
AgentSlot
├── inbox
└── execution
    ├── Idle(BaseAgent)
    └── Running(agent future + abort handle)
```

There is no parallel in-flight-agent table. Completing a future restores the
agent into the same slot; deleting the slot aborts and drops its running future.

## Turns and background work

A session has at most one root-visible turn:

```text
User Message
  -> Open Turn(TurnOrigin::User)
  -> Root Input
  -> Root Iteration(s)
  -> TurnEnded

Background Result(s)
  -> Root Inbox
  -> Root Busy
  -> Drain Root Inbox into Current Turn (no new Turn)
  -> Root Iteration(s)
  -> TurnEnded

Background Result(s)
  -> Root Inbox
  -> Root Idle
  -> Open Turn(TurnOrigin::Subagent)
  -> Drain Root Inbox into New Turn
  -> Root Iteration(s)
  -> TurnEnded
```

`subagent_spawn(foreground: true)` keeps the current tool call and turn pending
until the child completes. `foreground: false` returns the child id immediately.
The session actor keeps polling that child in the background and may accept a
new user turn while no root turn is active. Background results queue in the
parent slot. A result reaching a busy root becomes its next input inside the
active turn; a result reaching an idle root wakes it and opens a subagent-origin
turn. At either boundary, every result already queued in that inbox is activated
as one batch. They never create overlapping root turns.

Followup is live-only. It aborts a running child at a safe boundary and delivers
a new `Message` to the same in-memory agent. Completed children are removed;
there is no transcript-based resurrection path.

## Input inside a turn

Approval is an input boundary inside the active turn, not another turn:

```text
Root requests permission
  -> SessionEvent::InputRequested(request_id, PermissionApproval)
  -> Caller presents it using its own UI
  -> SessionControl::respond(request_id, Message)
  -> Resume Root in the same Turn
```

`submit` is accepted only when no root turn is active. While a turn is awaiting
input, only `respond` with its current request id can resume it; a stale id is
rejected. Core owns the semantic request and its durable id. The caller owns
presentation, so an IM adapter can make it look like ordinary conversation
without forcing that representation on CLI or GUI callers. If a background
subagent requests input while the root is idle, the actor opens a
`TurnOrigin::Subagent` turn for that request; an approval reached during an
active turn stays inside that turn.

## Interrupt, cancel, and close

- Interrupt ends the active turn, preserves live subagent instances, and resets
  them to an idle boundary.
- Cancel ends the active turn and removes spawned subagents, including detached
  background work.
- Close cancels outstanding work, emits `SessionEvent::Closed`, checkpoints a
  persistent session, and invalidates that control lease. The session id remains
  registered and can be opened again.
- Delete closes any open actor, removes the session id, and removes its runtime
  checkpoint batch.

## Persistence

The current checkpoint format is intentionally not compatible with the removed
`SessionDrive` / `OrchestratorInstance` layout.

```text
orchestrator[1]
└── agent-id-allocator

session-runtime[session_id]
├── session-state
└── multiagent-runtime
    ├── graph / ready queue / approvals
    └── AgentSlot[*]
        ├── inbox
        └── BaseAgent durable parts

session-registry[1]
└── session-store
```

`session-state` and `multiagent-runtime` are written in one batch. A running
agent future cannot be serialized, so `SessionActor` defers a requested
checkpoint until every affected slot is back at a checkpoint-safe boundary.
Close always stops work before its final checkpoint.

Transcripts, profile documents, long-term memory, and skills remain owned by
their file-backed stores rather than being embedded in the session checkpoint.
