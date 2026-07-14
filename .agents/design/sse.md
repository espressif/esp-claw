# Session Event Stream (SSE-ready)

`AgentSystem::open_session` returns a [`SessionControl`, `SessionEventStream`]
pair. The stream remains open across submits; each submit creates one turn on
that stream. Pulling `SessionEventStream` drives the session and yields events as
they happen.

## Event model

```rust
pub enum StreamPart<T> {
    Delta(T),
    End,
}

pub enum SessionEvent {
    TurnStarted { turn: TurnId },
    IterationStarted { iteration: IterationId },

    Reasoning(StreamPart<String>),
    Output(StreamPart<String>),
    ToolCalls(StreamPart<ToolCall>),

    IterationEnded,
    TurnEnded { turn: TurnId },
    Error { message: String },
    Closed,
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
TurnStarted { turn: 1 }
  IterationStarted { iteration: 1 }
    Reasoning(Delta("..."))
    Reasoning(End)
    Output(End)
    ToolCalls(Delta(call))
    ToolCalls(End)
  IterationEnded
  IterationStarted { iteration: 2 }
    Reasoning(End)
    Output(Delta("done"))
    Output(End)
    ToolCalls(End)
  IterationEnded
TurnEnded { turn: 1 }

TurnStarted { turn: 2 }
  ...
TurnEnded { turn: 2 }

Closed
```

Messages synthesized outside the LLM stream, such as approval prompts,
clarifications, terminal tool messages, and failure text, are also emitted as
`Output(Delta)*` followed by `Output(End)`. They can appear at turn scope rather
than inside an iteration. `Closed` is terminal for the session stream;
`TurnEnded` is not.

## Scope and ownership

Only root-agent iterations are externally visible. Subagent events use a
disabled sink and remain internal. Root iterations are sequential, so the
`IterationStarted..IterationEnded` bracket supplies enough scope for content
events without repeating agent or iteration ids on every delta.

The iteration loop owns LLM deltas and their three content boundaries. The
orchestrator owns turn boundaries and output synthesized outside the LLM
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

The current C payload for a tool call remains its name; Rust callers receive
the complete `ToolCall`. End events have null `text` and `error_message`.
