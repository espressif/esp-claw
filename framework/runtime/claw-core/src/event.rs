//! The event vocabulary a session event stream yields.
//!
//! One open session has one long-lived stream. User submits and background
//! subagent completions both create root-visible turns on that stream. Only the
//! **root** agent is externally visible, and a root's iterations are sequential,
//! so events carry no agent id: the `iteration` id is emitted once (on
//! [`SessionEvent::IterationStarted`]) and the following content events belong
//! to it by position.
//!
//! See `.agents/design/sse.md` for the full model (ordering, SSE forward-compat).

use crate::agent::IterationId;
use crate::session::TurnId;

// The reasoning cap is a compile-time tier, not a runtime knob. Exactly one of
// the mutually-exclusive `reasoning_short` / `reasoning_medium` / `reasoning_long`
// Cargo features selects it; the default is `reasoning_short`. Reject zero or
// multiple so the cap is never ambiguous.
#[cfg(not(any(
    feature = "reasoning_short",
    feature = "reasoning_medium",
    feature = "reasoning_long",
)))]
compile_error!(
    "enable exactly one reasoning tier feature: `reasoning_short`, `reasoning_medium`, or `reasoning_long`"
);
#[cfg(any(
    all(feature = "reasoning_short", feature = "reasoning_medium"),
    all(feature = "reasoning_short", feature = "reasoning_long"),
    all(feature = "reasoning_medium", feature = "reasoning_long"),
))]
compile_error!(
    "enable only one reasoning tier feature: `reasoning_short`, `reasoning_medium`, or `reasoning_long`"
);

/// Byte budget for a [`SessionEvent::Reasoning`] payload.
///
/// Reasoning text can be very long; the stream truncates it to this cap to keep
/// event payloads bounded. Output is never truncated. The cap is chosen at
/// compile time by the reasoning tier feature (`reasoning_short` = 2000,
/// `reasoning_medium` = 8000, `reasoning_long` = 32000 bytes).
#[cfg(feature = "reasoning_short")]
const REASONING_EVENT_LIMIT: usize = 2_000;
#[cfg(all(feature = "reasoning_medium", not(feature = "reasoning_short")))]
const REASONING_EVENT_LIMIT: usize = 8_000;
#[cfg(all(
    feature = "reasoning_long",
    not(feature = "reasoning_short"),
    not(feature = "reasoning_medium"),
))]
const REASONING_EVENT_LIMIT: usize = 32_000;

/// Why a root-visible turn started.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TurnCause {
    /// A user input accepted through [`SessionControl::submit`](crate::SessionControl::submit).
    UserSubmit,
    /// A background subagent completed and made the root ready again.
    BackgroundResult,
}

/// One item in a session's event stream.
///
/// Content variants ([`Reasoning`](Self::Reasoning), [`Output`](Self::Output),
/// [`ToolCall`](Self::ToolCall)) are mutually exclusive per event and, within one
/// iteration, arrive in the order `reasoning -> output -> tool calls` (whichever
/// are present), followed by diagnostic usage when cache profiling is enabled.
/// `Reasoning`/`Output` `text` fields are **append fragments**
/// (streaming emits many, non-streaming one holding the whole string); each
/// tool call is emitted as its own complete [`ToolCall`](Self::ToolCall) event,
/// in call order.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SessionEvent {
    /// A root-visible turn started.
    TurnStarted {
        /// The session-local turn this bracket opens.
        turn: TurnId,
        /// What made this turn runnable.
        cause: TurnCause,
    },
    /// A root LLM round started. Carries the only iteration id on the stream.
    IterationStarted {
        /// The iteration this bracket opens.
        iteration: IterationId,
    },
    /// Model thinking text, truncated to the configured limit.
    Reasoning {
        /// A fragment of the reasoning text (concatenate across events).
        text: String,
    },
    /// Assistant-visible text: a plain-text answer, an `conversation_end` closing
    /// message, an approval prompt, or a clarification. Never truncated.
    Output {
        /// A fragment of the assistant-visible text (concatenate across events).
        text: String,
    },
    /// One complete tool call the model requested this iteration. Emitted once
    /// per call, in call order (after any `Output`).
    ToolCall {
        /// The tool name.
        name: String,
    },
    /// Provider token/cache counters for the completed LLM iteration.
    #[cfg(feature = "cache_profile")]
    Usage {
        /// Counters reported by the provider; individual fields may be absent.
        usage: claw_api::ApiUsage,
    },
    /// The current root iteration ended.
    IterationEnded,
    /// The turn ended.
    TurnEnded {
        /// The session-local turn this bracket closes.
        turn: TurnId,
    },
    /// This session work item failed.
    Error {
        /// A human-readable failure message.
        message: String,
    },
    /// The session was closed and no more events will be sent.
    Closed,
}

/// Where [`SessionEvent`]s are pushed while a session is driven.
///
/// Cheap to clone (an `Arc`-backed channel sender). A
/// [`disabled`](Self::disabled) sink drops every event — handed to subagents so
/// only the root's events reach the stream, and used when a session has no
/// live subscriber.
#[derive(Clone)]
pub struct EventSink {
    tx: Option<async_channel::Sender<SessionEvent>>,
}

impl EventSink {
    /// A sink that forwards events to `tx`, truncating reasoning to
    /// [`REASONING_EVENT_LIMIT`].
    pub(crate) fn new(tx: async_channel::Sender<SessionEvent>) -> Self {
        Self { tx: Some(tx) }
    }

    /// A sink that drops everything. Handed to non-root agents.
    pub(crate) fn disabled() -> Self {
        Self { tx: None }
    }

    /// Push one event. A no-op on a disabled sink or a closed channel.
    pub(crate) fn emit(&self, event: SessionEvent) {
        if let Some(tx) = &self.tx {
            let _ = tx.try_send(event);
        }
    }

    /// Emit one streamed reasoning fragment, enforcing [`REASONING_EVENT_LIMIT`]
    /// on the **accumulated** length across fragments. `emitted` is the running
    /// byte count for the current iteration (the caller owns it, resetting it per
    /// iteration). A no-op once the cap is reached, when disabled, or empty.
    pub(crate) fn emit_reasoning_fragment(&self, fragment: &str, emitted: &mut usize) {
        if self.tx.is_none() || fragment.is_empty() || *emitted >= REASONING_EVENT_LIMIT {
            return;
        }
        let remaining = REASONING_EVENT_LIMIT - *emitted;
        let mut end = remaining.min(fragment.len());
        while end > 0 && !fragment.is_char_boundary(end) {
            end -= 1;
        }
        if end == 0 {
            return;
        }
        *emitted += end;
        self.emit(SessionEvent::Reasoning {
            text: fragment[..end].to_string(),
        });
    }
}
