# Orchestrator worker and session actors

The runtime has one process-wide worker and one actor per live session. The
orchestrator is a public handle; it does not own turn state or agent graphs.

```text
caller
  │
  ├─ Orchestrator ── open/delete/stop ──▶ Engine worker
  │                                      ├─ SessionActor[session A]
  │                                      ├─ SessionActor[session B]
  │                                      └─ dormant restored actors
  │
  └─ SessionControl ── submit/control ──▶ one SessionActor
                         SessionEvent ◀──┘
```

## Ownership

### `Orchestrator`

The `Send + Sync` public handle owns:

- the durable session registry;
- the process-wide engine command sender;
- the worker lifetime;
- process-wide API configuration.

It creates, lists, opens, and deletes sessions. Opening a session returns a
`SessionControl` and a `SessionEventStream`.

### `Engine`

The engine is constructed inside the worker because agent runtimes are
single-threaded and `!Send`. It owns only process-wide coordination:

- the agent factory and global `AgentIdAllocator`;
- dormant restored session actors;
- active actor command senders and actor futures;
- `OpenSession`, `DeleteSession`, and `Stop` handling.

The engine cooperatively polls every actor future. It does not interpret
messages, create turns, schedule agents, or route subagent results.

### `SessionActor`

Each actor is the sole owner of one session's mutable runtime:

```text
SessionActor
├─ SessionState
│  ├─ active/pending turn input
│  ├─ next turn id
│  └─ reasoning effort
├─ RuntimeExecution
│  ├─ Idle(MultiagentRuntime)
│  └─ Driving { control, future }
├─ event sink and active lease
└─ checkpoint/control/close state
```

`SessionControl` sends `SessionCommand` directly to this actor. The engine is
not a relay for turn traffic.

The actor keeps polling commands while `MultiagentRuntime` is driving. This is
what permits a caller to submit a new foreground message while detached
subagents continue in the background. Interrupt, cancel, close, and delete are
also observed at cooperative drive boundaries.

Only one client may hold the open event stream for a session. Every open gets a
new lease; commands from an older lease are rejected after close and reopen.

## Turn and subagent semantics

- A user submission opens a `TurnOrigin::User` turn.
- `subagent_spawn(foreground = true)` waits inside the current tool call and
  returns the child result in the current turn.
- `subagent_spawn(foreground = false)` returns the child id immediately. When
  the child finishes, the session actor opens a `TurnOrigin::Subagent` turn and
  delivers the result to the parent through the multiagent runtime.
- `subagent_followup` is live-only. A completed child is gone and cannot be
  resumed from transcript state.
- Interrupt stops the foreground turn but preserves live detached children.
- Cancel stops the turn and all children owned by the session.

Detached work does not keep a user turn artificially open, and it does not make
the session API busy. The long-lived session event stream carries later
subagent-origin turns to the caller.

## Scheduling

There is one worker thread, but asynchronous work from different sessions is
interleaved cooperatively:

1. `EnginePoll` polls the process-wide command receiver.
2. It polls each active `SessionActor` future.
3. Each actor polls its direct command receiver and its current runtime future.
4. `MultiagentRuntime` owns the graph, ready queue, agent slots, and lifecycle
   transitions for that session.

No `Mutex` protects an agent graph, no stream drives the runtime, and no
instance is checked out of a shared map across an `await`.

## Persistence

The stable checkpoint boundary for a session is:

```text
session-state + multiagent-runtime = session-runtime[session_id]
```

A running agent tick owns its `BaseAgent` future and cannot be serialized. The
actor therefore defers a requested checkpoint until every `AgentSlot` is back
at a checkpoint-ready boundary. Close and shutdown stop work before publishing
the final session checkpoint.

## Dependency direction

```text
protocol   config   memory
    \        |       /
             agent
               ↑
           multiagent
               ↑
            session
               ↑
          orchestrator
```

`agent` is a single-agent runtime and has no knowledge of sessions,
orchestration, parent/child graphs, or subagent tools. `multiagent` adapts that
runtime into a graph and scheduler. `session` owns public turn semantics.
`orchestrator` only owns process-wide lifecycle and the worker boundary.
