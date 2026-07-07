# Submit Event Stream (SSE-ready)

`Orchestrator::submit` returns an async `Stream` of `AgentEvent` instead of a
single `DriveOutput`. The caller just keeps pulling until the stream ends. The
event vocabulary is shaped so a future token-level SSE transport slots in
without changing the enum or the caller.

## Event model

```rust
pub enum AgentEvent {
    TurnStarted,
    IterationStarted { iteration: IterationId },

    // Content: exclusive (one kind per event). `text` is an *append fragment*:
    // non-streaming emits one fragment = the whole string; future SSE emits many.
    Reasoning { text: String },        // model thinking, truncated to the cfg limit
    Output    { text: String },        // assistant-visible text, untruncated
    Tools     { names: Vec<String> },  // tool names invoked this iteration

    IterationEnded,
    TurnEnded,

    Error { message: String },         // this submit failed
}
```

- No `session` / `turn` fields: one `submit` == one session == one turn, so the
  stream itself is that scope.
- No `agent` / `depth` fields: only the **root** agent is externally visible
  (see scope), and a root's iterations are sequential, so there is no
  interleaving to disambiguate.
- `iteration` id is emitted **once**, on `IterationStarted`. The following
  `Reasoning` / `Output` / `Tools` / `IterationEnded` belong to it by position
  (the open `IterationStarted..IterationEnded` bracket).
- No separate `Reply` variant: every assistant-visible reply is an `Output`
  (see the emission table for the sources folded into it).

## Definitions

```
turn      = one submit = one delivered user message = one drive cycle
iteration = one LLM request/response round-trip (+ that round's tool execution)
```

A turn contains one or more iterations; the loop runs iterations until a round
returns a plain-text answer (no tools) or ends the conversation.

```
TurnStarted
  IterationStarted{it:1}  Reasoning  Tools   IterationEnded   // tool round
  IterationStarted{it:2}  Reasoning          IterationEnded   // final LLM round
  Output                                                      // routed answer (turn-level)
TurnEnded
```

`Reasoning` / `Tools` are **in-bracket** (emitted from the LLM round). `Output`
is **turn-level**: it is the *routed* reply, produced after a round yields, so it
appears after that round's `IterationEnded`. Consumers treat `Output` as "the
assistant-visible text of this turn" (concatenate); they do not depend on it
sitting inside a bracket. (Under SSE the final answer's `Output` would move into
the closing iteration's bracket as token fragments — see forward-compat.)

Code anchors: `BaseAgent::tick` = one iteration (`IterationLoop::run` once); a
tool round returns `TickOutcome::Working` (loop again = next iteration); a
plain-text round returns `TickOutcome::Yielded` (turn ends). `instance.turn`
counts turns.

## Ordering (what SSE actually delivers)

Within **one** API request/response, parts are monotonic and non-interleaving:

```
reasoning (one contiguous run, always first)  ->  msg?  ->  tools?
```

- `reasoning` is first and never returns after `msg`/`tools` start (same
  request). "reasoning again after a tool" only happens in the **next**
  iteration (a new request, after tool results are fed back).
- `msg` and `tools` may **both** be present in one request (model speaks, then
  calls tools); when both, `msg` precedes `tools`. They never co-occur at the
  same instant — SSE streams them sequentially (OpenAI/DeepSeek switch fields;
  Anthropic emits separate typed content blocks).
- Non-streaming today: the full `LlmResponse` carries all present fields at once
  (an accumulated snapshot); we emit them in the same order as separate events,
  so the consumer sees an ordering identical to the streaming case.

## SSE forward-compatibility

The hook is left open without adding fields:

1. **Content `text` = append fragment.** One-shot = a single fragment (the whole
   string); streaming = many fragments the consumer concatenates. `reasoning`
   truncation applies to the accumulated length.
2. **`Tools` carries only names.** Tool-call names arrive early in a stream
   (first chunk / block start); arguments stream later. Exposing arguments later
   is an additive `ToolArgs*` event, not a change here.
3. **Emission point moves down, links stay.** Today `Reasoning`/`Tools` are
   emitted from the full `LlmResponse` in `IterationLoop`, and the final answer's
   `Output` is emitted one level up when the round is routed. Under SSE, both
   emissions move into the transport's SSE parse loop (`claw-api` backend) — the
   answer's `Output` then lands inside the closing iteration's bracket as token
   fragments. The sink plumbing and the outward `Stream` are unchanged.

## Emission (root only)

| Event | When |
|---|---|
| `TurnStarted` / `TurnEnded` | first/last of one drive (one submit) |
| `IterationStarted` / `IterationEnded` | first/last of each root LLM round |
| `Reasoning` | root round's `LlmResponse.reasoning_content` (if any; cfg-truncated) |
| `Tools` | tool-call names of a root tool round |
| `Output` | root plain-text answer, `end_conversation` closing message, approval prompt, clarification |
| `Error` | task failure (`TickOutcome::Failed`) or submit precondition error (session not found) |

Not emitted (deliberately out of scope for now): a tool round's assistant
preamble text. Subagent events are never emitted (subagents are internal; their
output is not externally visible).

## Layering

```
IterationLoop (L3)  emits IterationStarted/Reasoning/Tools/IterationEnded via a
                    sink; it knows the iteration, not session/agent. (No Output:
                    the answer is a routed reply, emitted one level up.)
BaseAgent     (L2)  forwards the sink into IterationLoop (via Agent::tick).
instance      (L1)  hands an *active* sink only to the root agent's tick
                    (subagents get a no-op sink), so only root events reach the
                    stream. Stamps nothing extra (root is implicit).
orchestrator        opens TurnStarted, emits every routed root reply as Output
                    (plain answer / end / approval / clarify), Error on failure,
                    closes TurnEnded; owns the outward Stream.
```

The sink is an `async-channel::Sender<AgentEvent>` wrapper that is a no-op when
disabled (subagents). Reasoning truncation is a compile-time constant, not a
field it carries. The outward `Stream` is the paired `Receiver`
(`async-channel::Receiver` implements `Stream`).

## Implementation status

Landed (full stack):

- `AgentEvent` + `EventSink` (`claw-core/src/event.rs`). Reasoning is truncated to
  the compile-time `REASONING_EVENT_LIMIT` (selected by the mutually-exclusive
  `reasoning_short` / `reasoning_medium` / `reasoning_long` Cargo features,
  default `reasoning_short`) via `claw_utils::TruncatedText`.
- `Orchestrator::submit(self: &Arc<Self>, ..) -> SubmitStream`. Each submit owns
  one `async-channel`; the session shares one drive future (`Rc<RefCell<..>>`)
  that every live `SubmitStream` cooperatively advances. `TurnStarted`, `Output`
  (from every routed `RootReply`), `Error` (drive error / unknown session /
  superseded), and `TurnEnded` are emitted at the turn level.
- Iteration-level events: `&EventSink` is threaded `Agent::tick` → `BaseAgent` →
  `IterationLoop`, which emits `IterationStarted` (once, at the top), `Reasoning`
  (from `LlmResponse.reasoning_content`), `Tools` (root tool-round names), and
  `IterationEnded` (a `Drop` guard closes the bracket on every exit path).
  `instance` hands the live sink only to the **root** agent's tick; every subagent
  (and the internal approval resolver) ticks with a disabled sink, so only the
  root's iterations reach the stream.
- Call sites updated: `claw_agent::AgentSystem::submit`, the CLI, the example, and
  the FFI `run_submit` (drains the stream, joins `Output`, first `Error` fails).

## Rules

```rust
// One submit -> one Stream -> one turn -> one session. Caller drains it.
Orchestrator::submit(session, text, kind) -> impl Stream<Item = AgentEvent>

// Content events are exclusive and ordered within an iteration.
IterationStarted -> (Reasoning? -> Output? -> Tools?) -> IterationEnded

// text fields are append fragments (concatenate); non-streaming = one fragment.
// reasoning is truncated to the compile-time REASONING_EVENT_LIMIT; output is not.

// Only the root agent is visible. Subagent iterations are never emitted.
// iteration id appears once, on IterationStarted.
```
