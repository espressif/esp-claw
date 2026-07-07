//! The event vocabulary a [`submit`](crate::Orchestrator::submit) stream yields.
//!
//! One `submit` == one session == one turn, so the stream *is* that scope: events
//! carry no session/turn id. Only the **root** agent is externally visible, and a
//! root's iterations are sequential, so events carry no agent id either — the
//! `iteration` id is emitted once (on [`AgentEvent::IterationStarted`]) and the
//! following content events belong to it by position.
//!
//! See `.agents/design/sse.md` for the full model (ordering, SSE forward-compat).

use claw_utils::TruncatedText;

use crate::agent::IterationId;

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

/// Byte budget for a [`AgentEvent::Reasoning`] payload.
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

/// One item in a submission's event stream.
///
/// Content variants ([`Reasoning`](Self::Reasoning), [`Output`](Self::Output),
/// [`Tools`](Self::Tools)) are mutually exclusive per event and, within one
/// iteration, arrive in the order `reasoning -> output -> tools` (whichever are
/// present). The `text` fields are **append fragments**: non-streaming emits one
/// fragment holding the whole string; a future SSE transport emits many.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AgentEvent {
    /// The turn (one drive of one submitted message) started.
    TurnStarted,
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
    /// Assistant-visible text: a plain-text answer, an `end_conversation` closing
    /// message, an approval prompt, or a clarification. Never truncated.
    Output {
        /// A fragment of the assistant-visible text (concatenate across events).
        text: String,
    },
    /// Names of the tools invoked this iteration.
    Tools {
        /// Tool names, in call order.
        names: Vec<String>,
    },
    /// The current root iteration ended.
    IterationEnded,
    /// The turn ended.
    TurnEnded,
    /// This submit failed (a task failure or a submit precondition error).
    Error {
        /// A human-readable failure message.
        message: String,
    },
}

/// Where per-turn [`AgentEvent`]s are pushed while a submission is driven.
///
/// Cheap to clone (an `Arc`-backed channel sender). A
/// [`disabled`](Self::disabled) sink drops every event — handed to subagents so
/// only the root's events reach the stream, and used when a submission has no
/// live subscriber.
#[derive(Clone)]
pub struct EventSink {
    tx: Option<async_channel::Sender<AgentEvent>>,
}

impl EventSink {
    /// A sink that forwards events to `tx`, truncating reasoning to
    /// [`REASONING_EVENT_LIMIT`].
    pub(crate) fn new(tx: async_channel::Sender<AgentEvent>) -> Self {
        Self { tx: Some(tx) }
    }

    /// A sink that drops everything. Handed to non-root agents.
    pub(crate) fn disabled() -> Self {
        Self { tx: None }
    }

    /// Whether this sink forwards events. Lets emitters skip building payloads
    /// they would only drop (e.g. cloning tool names for a subagent).
    pub(crate) fn is_enabled(&self) -> bool {
        self.tx.is_some()
    }

    /// Push one event. A no-op on a disabled sink or a closed channel.
    pub(crate) fn emit(&self, event: AgentEvent) {
        if let Some(tx) = &self.tx {
            let _ = tx.try_send(event);
        }
    }

    /// Emit a [`AgentEvent::Reasoning`] with `full` truncated to
    /// [`REASONING_EVENT_LIMIT`]. A no-op when disabled or `full` is empty.
    pub(crate) fn emit_reasoning(&self, full: &str) {
        if self.tx.is_none() || full.is_empty() {
            return;
        }
        self.emit(AgentEvent::Reasoning {
            text: TruncatedText::with_limit(full, REASONING_EVENT_LIMIT).to_string(),
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn disabled_sink_drops_events() {
        let sink = EventSink::disabled();
        assert!(!sink.is_enabled());
        sink.emit(AgentEvent::TurnStarted);
        sink.emit_reasoning("thinking hard");
    }

    #[test]
    fn enabled_sink_forwards_and_truncates() {
        let (tx, rx) = async_channel::unbounded();
        let sink = EventSink::new(tx);
        sink.emit(AgentEvent::TurnStarted);
        let long = "a".repeat(REASONING_EVENT_LIMIT + 10);
        sink.emit_reasoning(&long);
        sink.emit_reasoning("");
        drop(sink);

        assert_eq!(rx.try_recv().unwrap(), AgentEvent::TurnStarted);
        // `TruncatedText` caps at the limit and appends "..." when it truncates.
        assert_eq!(
            rx.try_recv().unwrap(),
            AgentEvent::Reasoning {
                text: format!("{}...", "a".repeat(REASONING_EVENT_LIMIT))
            }
        );
        assert!(rx.try_recv().is_err());
    }
}
