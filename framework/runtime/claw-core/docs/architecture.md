# claw-core Architecture

## Request Path

```text
AgentSystem
  -> Orchestrator
  -> Engine
  -> OrchestratorInstance
  -> BaseAgent
  -> IterationLoop
  -> claw-api / claw-tool
```

A user submission enters through `SessionControl`, becomes a session turn, and
is delivered to the root agent. The root agent runs iterations until it yields,
ends, waits for approval, fails, or is interrupted.

## Roles

| Type | Role | Owned state |
| --- | --- | --- |
| `AgentSystem` | Public runtime entry point in `claw-agent`. Builds the runtime and exposes sessions and tools. | `ToolRegistry`, `Orchestrator` |
| `Orchestrator` | Thread-safe session facade. Sends commands to the worker engine and returns session controls and event streams. | Command sender, shared session view, API configuration |
| `Engine` | Runs all sessions on one worker thread and coordinates checkpoints. | Session runtimes, session registry, agent factory |
| `SessionRuntime` | Owns all engine-level state for one session and decides whether input/control can be accepted. | Session drive and its lazily-created agent instance |
| `SessionDrive` | Stores turn state and process-local I/O state behind `SessionRuntime`. | Active turn, turn counter, reasoning effort, event sink, control flags |
| `OrchestratorInstance` | Runs the agent graph for one session. Schedules agents and routes results, approvals, and graph effects. | Agent registry, graph, ready queue, in-flight tasks, pending approvals |
| `FsAgentFactory` | Resolves an agent manifest and assembles its tools, memory stores, skills, and context adapters. | Shared tool registry, skill registry, memory stores, API manager |
| `BaseAgent` | Concrete agent stored and driven by the session instance; executes one configured task as a sequence of ticks and iterations. | Transcript, context, tool view, adapters, LLM client, canonical task phase and mailbox |
| `IterationLoop` | Executes one LLM request and its optional tool calls. | Borrowed iteration inputs only |
| `ContextAdapter` | Projects memory, skills, and other sources into model context. | Adapter-specific state |
| `ClawApiManager` | Selects the LLM configuration for each runtime usage. | API configurations and usage bindings |

## Work Units

| Unit | Meaning |
| --- | --- |
| Session | Isolated conversation and agent graph. Created as persistent or ephemeral. |
| Turn | One user submission and all work it causes. It remains active while a task, result, or approval is pending. |
| Task | Work held by one agent until it ends, fails, or is cancelled. |
| Tick | One scheduler advance of an agent. |
| Iteration | One LLM request followed by zero or more tool calls. |

### Interrupt

- Ends the current turn.
- Stops the root at a safe boundary.
- Stops every active, ready, or queued subagent task.
- Keeps subagent instances and committed context and history; returns them to idle.
- Clears undelivered results owned by the turn.
- Returns after runtime cleanup.

### Cancel

- Ends the current turn.
- Immediately aborts the root and every subagent task.
- Deletes every subagent spawned by the root.
- Clears related queues, mailboxes, and undelivered results.
- Returns after runtime cleanup.

## Scheduling

`Engine` multiplexes `SessionRuntime` objects. Each `OrchestratorInstance`
multiplexes the root agent and its subagents. Each agent tick advances one task.
Background subagents keep the current turn active and deliver their results
through the session agent graph.

## Persistence

A persistent session checkpoints its lifecycle and runtime state and writes its
root transcript to storage. An ephemeral session keeps the same multi-turn
context in memory and disappears when the process stops.

| Persistent session state | Process-local state |
| --- | --- |
| Session registry entry | Ephemeral session registry entry |
| Active turn id and pending input | Event sink and control acknowledgements |
| Next turn id and reasoning effort | Running flags, abort handles, wakers, futures |
| Agent graph, ready queue, approvals, mailbox | Graph effects and snapshot cache |
| Agent state and durable agent parts | Factories, clients, transports, timers |
| Root transcript | Ephemeral root transcript |

Turn acceptance and interrupt/cancel completion follow the live runtime state.
The runtime attempts a checkpoint after submit and terminal cleanup. Failure is
reported as persistence degradation and retried at a later checkpoint boundary.
Persistent session creation and deletion immediately attempt to publish the session registry.

Session close waits for its final checkpoint. The session remains closed if that
checkpoint fails, and close returns a persistence error. Recovery starts from
the latest successful checkpoint and may repeat later work.
