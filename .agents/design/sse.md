# Session Event Stream (SSE-ready)

`AgentSystem::open_session` returns a [`SessionControl`, `SessionEventStream`]
pair. The stream remains open across submits. A submit creates a user-origin
turn. A detached subagent result joins an active root turn, or creates a
subagent-origin turn on the same stream when the root is idle.

## Event model

```rust
pub enum StreamPart<T> {
    Delta(T),
    End,
}

pub enum SessionEvent {
    TurnStarted { turn: TurnId, origin: TurnOrigin },
    InputRequested { request: InputRequestId, kind: InputRequestKind },
    IterationStarted { iteration: IterationId },

    Reasoning(StreamPart<String>),
    Output(StreamPart<String>),
    ToolCalls(StreamPart<ToolCall>),

    IterationEnded,
    TurnEnded { turn: TurnId },
    Error { message: String },
    Closed,
}

pub enum InputRequestKind {
    PermissionApproval { summary: String },
}

pub enum TurnOrigin {
    User,
    Subagent { agent: AgentId },
}
```

`Reasoning` and `Output` deltas are append fragments. Each `ToolCalls` delta is
one complete `ToolCall`, including its provider id, name, and complete JSON
arguments. `End` is a boundary event, not a success or error status.

## Iteration ordering

The three content streams are contiguous and explicitly closed in every root
LLM iteration:

```text
IterationStarted
  Reasoning(Delta)*
  Reasoning(End)
  Output(Delta)*
  Output(End)
  ToolCalls(Delta)*
  ToolCalls(End)
IterationEnded
```

Exactly one `End` is emitted for each content kind, including a kind with no
deltas. A caller therefore never has to infer that reasoning, output, or tool
calls ended from the next event or from `IterationEnded`.

The lower-level `LlmDelta` contract has the same monotonic ordering. A tool call
is emitted only after all of its arguments have arrived, so `ToolCalls(Delta)`
never contains a partial call. The content `End` events are emitted as soon as
the LLM stream finishes and before tool execution. `IterationEnded` is emitted
after the rest of the iteration finishes. Error, cancellation, and interruption
paths still close every open content stream before `IterationEnded`.

## Turn ordering

One long-lived session stream can carry multiple turns:

```text
TurnStarted { turn: 1, origin: User }
  IterationStarted { iteration: 1 }
    Reasoning(Delta("..."))
    Reasoning(End)
    Output(End)
    ToolCalls(Delta(call))
    ToolCalls(End)
  IterationEnded
  InputRequested {
    request: input-1,
    kind: PermissionApproval { summary: "..." },
  }
  // caller: SessionControl::respond(input-1, message)
  IterationStarted { iteration: 2 }
    Reasoning(End)
    Output(Delta("done"))
    Output(End)
    ToolCalls(End)
  IterationEnded
TurnEnded { turn: 1 }

TurnStarted { turn: 2, origin: Subagent { agent: agent-2 } }
  ...
TurnEnded { turn: 2 }

Closed
```

An input request pauses, but does not end, the current turn. `submit(message)`
only starts a new user-origin turn while the session is idle.
`respond(request_id, message)` resumes the active turn and rejects a missing or
stale request id. If the response does not resolve the request, the actor emits
a new `InputRequested` with a new id in that same turn.

An input request reached while a root turn is active stays in that turn. If an
idle root is woken by a background subagent that needs input, the actor opens a
`TurnOrigin::Subagent` turn and emits the request there.

Input request presentation belongs to the caller. Core emits semantic data and
never turns an approval prompt or clarification into `Output`; a chat caller
may display the request as ordinary assistant text, while a GUI may render a
dialog or buttons. Terminal tool messages and failure text may still appear as
turn-scope `Output(Delta)*` followed by `Output(End)`. `Closed` is terminal for
the session stream; `TurnEnded` is not.

`subagent_spawn(foreground: true)` waits inside its tool call, so its result
stays in the current turn. `subagent_spawn(foreground: false)` returns the agent
id immediately; the current turn may end while that agent continues running.
When the detached result reaches a root with an active turn, it becomes a later
root input inside that turn before `TurnEnded`. When it reaches an idle root,
the session actor opens a new `TurnOrigin::Subagent` turn. A caller may submit
another user message while only detached work is running; that message opens an
independent user-origin turn.

## Scope and ownership

Only root-agent iterations are externally visible. Subagent iterations use a
disabled sink and remain internal; a detached result becomes root input in the
active turn or wakes an idle root into a new turn. Root iterations are
sequential, so the
`IterationStarted..IterationEnded` bracket supplies enough scope for content
events without repeating agent or iteration ids on every delta.

The iteration loop owns LLM deltas and their three content boundaries. The
session actor owns turn boundaries and output synthesized outside the LLM
stream. The outward `SessionEventStream` wraps the session receiver and is the
only public read side.

Reasoning is capped by the selected compile-time feature
(`reasoning_short`/`reasoning_medium`/`reasoning_long`) across all reasoning
deltas in one iteration. Output and tool calls are not truncated.

## C ABI mapping

The C ABI preserves the existing content event numbers and adds explicit end
kinds:

| Rust event | C event kind |
|---|---|
| `Output(Delta(text))` | `CLAW_AGENT_EVENT_KIND_OUTPUT` |
| `Output(End)` | `CLAW_AGENT_EVENT_KIND_OUTPUT_END` |
| `Reasoning(Delta(text))` | `CLAW_AGENT_EVENT_KIND_REASONING` |
| `Reasoning(End)` | `CLAW_AGENT_EVENT_KIND_REASONING_END` |
| `ToolCalls(Delta(call))` | `CLAW_AGENT_EVENT_KIND_TOOLS` |
| `ToolCalls(End)` | `CLAW_AGENT_EVENT_KIND_TOOLS_END` |
| `InputRequested { request, PermissionApproval { summary } }` | `CLAW_AGENT_EVENT_KIND_INPUT_REQUESTED` |

The current C payload for a tool call remains its name; Rust callers receive
the complete `ToolCall`. For `INPUT_REQUESTED`, `request_id` is non-zero,
`input_kind` is `PERMISSION_APPROVAL`, and `text` carries the semantic summary;
the caller replies through `claw_agent_session_respond`. End events have null
`text` and `error_message`. The C ABI currently skips `TurnStarted`, so
`TurnOrigin` is exposed by the Rust public API only.
