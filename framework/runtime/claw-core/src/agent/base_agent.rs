//! Layer 2 base agent: a black box that takes **commands** in and reports an
//! outcome out, driven one iteration per [`tick`](BaseAgent::tick).
//!
//! # Command in, outcome out
//!
//! Everything entering the agent is an [`AgentCommand`]. Convenience methods
//! ([`run`](BaseAgent::run), [`append_message`](BaseAgent::append_message),
//! [`cancel`](BaseAgent::cancel), …) are thin wrappers that only
//! [`send_command`](BaseAgent::send_command) — they hold no state logic of their
//! own. Commands queue on an inbox and are reduced by the single funnel
//! `apply_inbound`. Each [`tick`](BaseAgent::tick) returns one [`TickOutcome`]:
//! the agent never has a side-channel of events, because everything it has to
//! report coincides with the moment a tick hands control back.
//!
//! This keeps the agent a uniform unit suitable as the core of a multi-agent
//! system: an orchestrator drives many agents through the identical
//! `send_command` / `tick` triple and never reaches into their internals.
//!
//! # Driving
//!
//! [`tick`](BaseAgent::tick) returns what happened this tick. `Working` means
//! "pump again now"; `Idle` means "nothing to do, wait for a command"; every
//! other variant is a result the driver acts on:
//!
//! ```ignore
//! loop {
//!     match agent.tick() {
//!         TickOutcome::Working => continue,
//!         TickOutcome::Idle => wait_for_command(),
//!         TickOutcome::Yielded { text } => { print(text); wait_for_command(); }
//!         TickOutcome::AwaitingApproval { id, .. } => decide(id),
//!         TickOutcome::Ended { final_message } => { print(final_message); break; }
//!         TickOutcome::Cancelled { .. } => break,
//!         TickOutcome::Failed(error) => { report(error); break; }
//!     }
//! }
//! ```
//!
//! # Termination
//!
//! A plain-text answer is [`Yielded`](TickOutcome::Yielded) — **non-terminal** —
//! and the agent goes idle awaiting the next message. A task ends only when the
//! agent decides so itself (the built-in `end_conversation` tool →
//! [`Ended`](TickOutcome::Ended)), when the orchestrator hard-stops it
//! ([`Cancel`](AgentCommand::Cancel) → [`Cancelled`](TickOutcome::Cancelled)), or
//! on [`Failed`](TickOutcome::Failed). The outside never preempts the agent's
//! reasoning; it only appends information and lets the agent re-decide. A terminal
//! outcome is reported once and leaves the agent **idle and reusable** — the next
//! [`AppendMessage`](AgentCommand::AppendMessage) starts a fresh task over the
//! same memory and identity.

use std::collections::{HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use claw_api::{ChatError, ClawApi, RetryPolicy};
use claw_interface::http::ClawHttp;
use claw_interface::ClawFs;
use claw_memory::TranscriptStore;
use serde_json::Value;

use super::iteration_loop::{
    ChatMessages, CompletedKind, CompletedOutcome, InterruptionControl, IterationId, IterationLoop,
    IterationLoopError, IterationOutcome, IterationResult, IterationStep, PreemptedOutcome,
    SystemPrompt, ToolRun,
};
use crate::agent::tools::{internal_tool_group, ControlSignal, ControlSink};
use crate::memory::{
    ContextAdapter, ContextAdapterInput, History, RecentMessagesContextAdapter,
    SkillContextAdapter, SkillTools, SummaryCursor, ToolPolicyContextAdapter, Transcript,
};
use claw_context::{Block, BlockKind, Context};
use claw_permission::{Grant, PermissionPolicy};
use claw_skill::SkillSet;
use claw_tool::{
    BlockPolicy, PermissionGate, ToolBlockVerdict, ToolGate, ToolSet, ToolSetError,
    DEFAULT_BLOCK_RETRIES,
};

crate::define_prefixed_id!(AgentId, "agent-", "agent");
crate::define_prefixed_id!(ApprovalId, "approval-", "approval");

// ===========================================================================
// Public command / outcome vocabulary
// ===========================================================================

/// Inbound: a control input handed to the agent. This is the agent's entire
/// external surface — the outside drives the agent only through these.
///
/// Notably there is **no `Preempt`**: outside input never ends or interrupts the
/// agent's reasoning, it only adds information ([`AppendMessage`](Self::AppendMessage))
/// and lets the agent re-decide. Hard termination is [`Cancel`](Self::Cancel).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AgentCommand {
    /// Append a user message. Starts a fresh task when the agent is idle;
    /// otherwise it joins the in-progress task.
    AppendMessage(String),
    /// Abandon the current task. (Orchestrator-initiated hard stop — distinct from
    /// the agent ending itself via `end_conversation`.) Being disruptive, it
    /// commits the abandoned turn and records an interruption marker (keyed on the
    /// [`CancelReason`]) in memory, so the next task does not inherit an
    /// unexplained, half-finished exchange.
    Cancel {
        /// Why the task is being abandoned; selects the recorded interruption marker.
        reason: CancelReason,
    },
    /// Stop scheduling iterations until [`Resume`](Self::Resume). No-op unless the
    /// agent is actively running.
    Pause,
    /// Resume a [`Pause`](Self::Pause)d agent.
    Resume,
    /// Deliver a human decision for a pending [`TickOutcome::AwaitingApproval`].
    /// Ignored unless the agent is awaiting this exact approval.
    ApprovalResult {
        /// The pending approval this decision answers.
        id: ApprovalId,
        /// The human's verdict.
        decision: ApprovalDecision,
    },
}

/// The agent's externally observable lifecycle state.
///
/// Exposed so a driver can read which state a rejected command hit off an
/// [`AgentCommandError`]. `Idle` means "no active task, awaiting input" — both
/// before the first task and after one finishes (terminal outcomes leave the
/// agent idle and reusable).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AgentState {
    /// No active iteration; waiting for an [`AppendMessage`](AgentCommand::AppendMessage).
    Idle,
    /// A task is actively iterating.
    Running,
    /// A running task whose iteration scheduling is [`Pause`](AgentCommand::Pause)d.
    Paused,
    /// Paused on a permission-policy `Ask`, awaiting an
    /// [`ApprovalResult`](AgentCommand::ApprovalResult).
    AwaitingApproval,
}

/// Rejection of an [`AgentCommand`] that is invalid for the agent's current
/// [`AgentState`].
///
/// The agent is a state machine; not every command is meaningful in every
/// state (e.g. [`Resume`](AgentCommand::Resume) after a
/// [`Cancel`](AgentCommand::Cancel) left the agent idle). A rejected command is
/// **not** enqueued and the agent is left unchanged, so the caller can react
/// without racing a `tick`. Validation is against the state the agent *will* be
/// in once already-queued commands are applied, so batching commands between
/// ticks is sound.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum AgentCommandError {
    /// [`Pause`](AgentCommand::Pause) is only valid while
    /// [`Running`](AgentState::Running).
    #[error("cannot pause: the agent is {state:?}, not running")]
    CannotPause {
        /// The state the agent was in when the pause was rejected.
        state: AgentState,
    },
    /// [`Resume`](AgentCommand::Resume) is only valid while
    /// [`Paused`](AgentState::Paused).
    #[error("cannot resume: the agent is {state:?}, not paused")]
    CannotResume {
        /// The state the agent was in when the resume was rejected.
        state: AgentState,
    },
    /// [`Cancel`](AgentCommand::Cancel) has nothing to act on while
    /// [`Idle`](AgentState::Idle).
    #[error("cannot cancel: the agent is idle with no active task")]
    NothingToCancel,
    /// [`ApprovalResult`](AgentCommand::ApprovalResult) is only valid while
    /// [`AwaitingApproval`](AgentState::AwaitingApproval).
    #[error("cannot resolve approval: the agent is {state:?}, not awaiting approval")]
    NotAwaitingApproval {
        /// The state the agent was in when the approval result was rejected.
        state: AgentState,
    },
    /// The agent is awaiting approval, but for a different request id.
    #[error("approval {got} does not match the pending approval {expected}")]
    ApprovalMismatch {
        /// The approval the agent is actually waiting on.
        expected: ApprovalId,
        /// The approval id the caller supplied.
        got: ApprovalId,
    },
}

/// Why a task was [`Cancel`](AgentCommand::Cancel)led, carried on the resulting
/// [`TickOutcome::Cancelled`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CancelReason {
    /// A human asked to stop.
    UserRequested,
    /// The orchestrator replaced this task with a newer one.
    Superseded,
    /// The host is shutting the agent down.
    Shutdown,
}

/// A human's answer to an approval request.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ApprovalDecision {
    /// The human approved; the agent resumes and proceeds.
    Approved,
    /// The human rejected, with a reason recorded for the agent to reconsider.
    Rejected(String),
}

/// What one [`tick`](BaseAgent::tick) did — the agent's sole output channel.
///
/// `Working`/`Idle` are liveness for the driver loop; the rest are one-shot
/// results reported on the tick that produced them. A single tick yields exactly
/// one of these (tool execution is internal — it shows up only as `Working`).
#[derive(Clone, Debug)]
#[must_use]
pub enum TickOutcome {
    /// Progress was made; call `tick` again promptly.
    Working,
    /// Nothing to do right now (waiting for input, paused, or awaiting approval).
    Idle,
    /// The model returned a user-facing answer and handed control back.
    /// **Non-terminal** — the agent goes idle awaiting the next message.
    Yielded {
        /// The model's user-facing answer.
        text: String,
    },
    /// A tool call's permission policy returned `Ask`; the agent is paused for a
    /// human decision. Resolve it with [`resolve_approval`](BaseAgent::resolve_approval).
    AwaitingApproval {
        /// The id to pass back via [`resolve_approval`](BaseAgent::resolve_approval).
        id: ApprovalId,
        /// A human-readable description of what needs approving.
        summary: String,
    },
    /// Terminal: the agent ended the task itself (via `end_conversation`). The
    /// agent returns to idle and may be re-tasked.
    Ended {
        /// The agent's closing message.
        final_message: String,
    },
    /// Terminal: the task was cancelled by the orchestrator.
    Cancelled {
        /// Why the task was cancelled.
        reason: CancelReason,
    },
    /// Terminal: the task failed.
    Failed(AgentRunError),
}

impl TickOutcome {
    /// True for the terminal outcomes ([`Ended`](Self::Ended) /
    /// [`Cancelled`](Self::Cancelled) / [`Failed`](Self::Failed)) — the task is
    /// over, though the agent stays reusable.
    ///
    /// # Examples
    ///
    /// ```
    /// use claw_core::agent::{CancelReason, TickOutcome};
    ///
    /// assert!(TickOutcome::Ended { final_message: "done".into() }.is_terminal());
    /// assert!(TickOutcome::Cancelled { reason: CancelReason::Shutdown }.is_terminal());
    /// assert!(!TickOutcome::Working.is_terminal());
    /// assert!(!TickOutcome::Yielded { text: "partial answer".into() }.is_terminal());
    /// ```
    pub fn is_terminal(&self) -> bool {
        matches!(
            self,
            TickOutcome::Ended { .. } | TickOutcome::Cancelled { .. } | TickOutcome::Failed(_)
        )
    }
}

/// Cause of a terminal [`TickOutcome::Failed`].
///
/// Wraps the lower-level errors a tick can hit: a failed LLM/tool iteration, or a
/// tool refused past the soft-hide retry budget. Context assembly is driven by
/// adapters; adapter-local failures are logged at the adapter boundary, and
/// [`Context::request`] is infallible, so the tick never fails on context
/// assembly.
#[derive(Clone, Debug, thiserror::Error)]
pub enum AgentRunError {
    /// Request history was not a JSON array.
    #[error("messages must be a JSON array")]
    MessagesNotArray,
    /// The LLM returned tool calls without the raw assistant message needed for
    /// transcript persistence.
    #[error("LLM tool-call response missing raw assistant message JSON")]
    MissingAssistantMessage,
    /// The raw assistant message returned by the LLM was not valid JSON.
    #[error("LLM raw assistant message JSON is not valid JSON")]
    MalformedAssistantMessage,
    /// The LLM issued tool calls, but this agent was built without a tool port.
    #[error("LLM issued tool calls but no tools port was supplied")]
    MissingTools,
    /// The LLM chat request failed.
    #[error(transparent)]
    Chat(#[from] ChatError),
    /// The model kept calling a tool that soft-hide gating does not permit this
    /// phase, past the allowed retry budget (the agent's
    /// [`BlockPolicy`](claw_tool::BlockPolicy)).
    #[error("tool not permitted in the current phase: {name}")]
    ToolNotPermitted {
        /// The name of the refused tool.
        name: String,
    },
}

impl From<IterationLoopError> for AgentRunError {
    fn from(error: IterationLoopError) -> Self {
        match error {
            IterationLoopError::MessagesNotArray => Self::MessagesNotArray,
            IterationLoopError::MissingAssistantMessage => Self::MissingAssistantMessage,
            IterationLoopError::MalformedAssistantMessage => Self::MalformedAssistantMessage,
            IterationLoopError::MissingTools => Self::MissingTools,
            IterationLoopError::Chat(error) => Self::Chat(error),
        }
    }
}

/// Failure assembling a [`BaseAgent`] in [`BaseAgentBuilder::build`].
#[derive(Clone, Debug, thiserror::Error)]
pub enum BaseAgentBuildError {
    /// Merging the built-in tool group onto the caller's tools hit a name clash.
    #[error(transparent)]
    Tools(#[from] ToolSetError),
    /// Tools were provided but the configured LLM does not support tool calls, so
    /// they could never be used — surfaced rather than silently dropped.
    #[error("tools were provided but the configured LLM does not support tools")]
    ToolsUnsupported,
}

// ===========================================================================
// Internals
// ===========================================================================

/// One item on the agent's inbox: either an external [`AgentCommand`] or an
/// internal [`ControlSignal`] raised by a built-in tool. Both flow through the
/// one reducer, but only `Command` is constructible by outside callers.
enum Inbound {
    Command(AgentCommand),
    Control(ControlSignal),
}

/// A cloneable handle to abort an agent's in-flight LLM/tool round from another
/// task.
///
/// Obtain it via [`BaseAgent::abort_handle`] **before** the tick loop (you cannot
/// borrow the agent while a `tick` holds `&mut self`). It shares the same
/// `Arc<AtomicBool>` the internal iteration loop polls at its checkpoints, so it can
/// stop a `tick` blocked on the LLM HTTP. It is plumbing for stopping a now-stale
/// call — the *content* of the new input still arrives as an [`AgentCommand`].
#[derive(Clone)]
pub struct AgentAbortHandle {
    flag: Arc<AtomicBool>,
}

impl AgentAbortHandle {
    /// Abort the in-flight (or next) iteration at its next checkpoint.
    pub fn abort(&self) {
        self.flag.store(true, Ordering::Release);
    }
}

/// The agent's own abort flag, fed to the per-tick [`IterationLoop`].
struct AgentInterruption {
    flag: Arc<AtomicBool>,
}

impl InterruptionControl for AgentInterruption {
    fn interrupt_flag(&self) -> &Arc<AtomicBool> {
        &self.flag
    }
}

// ===========================================================================
// BaseAgent
// ===========================================================================

/// A base agent that runs one task at a time as a sequence of iterations.
///
/// Build once via [`BaseAgent::builder`]; then drive it with commands and ticks.
/// The agent is long-lived and reused across tasks — its conversation memory and
/// identity persist, so finishing a task leaves it ready for the next.
///
/// # Examples
///
/// ```ignore
/// let mut agent = BaseAgent::builder(llm, memory)
///     .with_system_prompt("You are a helpful assistant.")
///     .with_tools(tools)
///     .build()?;
///
/// agent.run("summarize today's news");
/// loop {
///     match agent.tick() {
///         TickOutcome::Working => continue,
///         TickOutcome::Yielded { text } => { println!("{text}"); break; }
///         TickOutcome::Ended { final_message } => { println!("{final_message}"); break; }
///         TickOutcome::Failed(error) => return Err(error.into()),
///         _ => break,
///     }
/// }
/// ```
pub struct BaseAgent<H: ClawHttp> {
    llm: ClawApi<H>,
    /// Retry policy applied to every per-iteration LLM call.
    retry_policy: RetryPolicy,
    interruption: AgentInterruption,
    /// The conversation transcript's sole owner. The agent **writes** it directly
    /// at each boundary (a user message, a committed answer/tool patch, an
    /// end/cancel marker) and **reads** it to assemble each request
    /// ([`run_iteration`](Self::run_iteration)); it also lends the read view
    /// ([`Transcript::as_history`]) to each [`ContextAdapter`] so they can pull
    /// from it.
    /// Held behind the [`Transcript`] trait object — the agent never sees the
    /// concrete conversation-memory type, which is why it is not generic over a
    /// filesystem.
    transcript: Arc<dyn Transcript>,
    /// The agent's tools, including soft-hide phase gating (which tools may run
    /// now via [`ToolSet::is_allowed`]). The full schema is always sent
    /// regardless, so the cached prompt prefix stays stable. The agent only reads
    /// the set's two prompt surfaces (placed via the `context` cache).
    tools: Option<ToolSet>,
    /// The soft-hide "retry then fail" streak counter. Kept beside the tool set
    /// (not inside it) because it is conversation state, while [`ToolSet`] owns the
    /// immutable catalog and the once-rendered, cached wire surfaces. Fed each tool
    /// round by [`apply_tool_block_policy`](Self::apply_tool_block_policy).
    block_policy: BlockPolicy,
    /// The agent's context assembly, owned wholesale by `claw-context`: inherited
    /// blocks, the agent instruction, adapter-projected blocks/reminders, the
    /// cached system prefix, and the ephemeral reminder tail. The agent does not
    /// hand-place adapter sources; they contribute into a context sink, and
    /// [`Context::request`] renders lazily. Change detection, wire ordering, and
    /// reminder rendering all live in the context.
    context: Context,
    /// The permission gate consulted per tool call (`None` = no permission layer:
    /// every call that passes soft-hide runs). A `claw-tool` type owning the
    /// grant store of human decisions; mutated when an
    /// [`ApprovalResult`](AgentCommand::ApprovalResult) resolves a pending ask.
    gate: Option<PermissionGate>,
    /// Action signatures awaiting the current human decision — the calls the
    /// permission policy asked about this tick. Recorded into the gate's grant
    /// store when the [`ApprovalResult`](AgentCommand::ApprovalResult) arrives,
    /// then cleared.
    pending_grant_signatures: Vec<String>,
    next_iteration: IterationId,
    next_approval: usize,
    pending_approval: Option<ApprovalId>,
    /// The committed lifecycle state, advanced as the inbox is drained in `tick`.
    lifecycle: AgentState,
    /// The lifecycle state the agent *will* be in once every command already on
    /// the inbox is applied. Commands are validated against this (not `lifecycle`)
    /// so a batch enqueued between ticks is checked in order; it is reset to
    /// `lifecycle` at the end of each `tick`. No `tick` can run between two
    /// `send_command` calls (both need `&mut self`), so this is the only thing
    /// that moves the lifecycle between ticks and the projection stays exact.
    projected_lifecycle: AgentState,
    /// The actionable outcome produced during the current tick, if any. Reset at
    /// the start of each tick; a single tick produces at most one.
    outcome: Option<TickOutcome>,
    /// Sink the built-in tools push [`ControlSignal`]s onto; drained each tick.
    control: ControlSink,
    /// Registered context adapters. Each pulls the transcript and contributes
    /// context (blocks and/or messages) before every iteration, and may have added
    /// its tools to [`tools`](Self::tools) at registration. The conversation
    /// transcript is **not** here (it is [`transcript`](Self::transcript), the
    /// thing these adapters read *from*); these are pure readers (e.g. long-term
    /// memory) added via [`register_context_adapter`](Self::register_context_adapter).
    /// Owned (not shared) so they can be refreshed under `&mut`; driven only from
    /// this tick thread.
    adapters: Vec<Box<dyn ContextAdapter>>,
    inbox: VecDeque<Inbound>,
}

fn install_context_adapter(
    adapters: &mut Vec<Box<dyn ContextAdapter>>,
    tools: &mut Option<ToolSet>,
    adapter: Box<dyn ContextAdapter>,
) -> Result<(), BaseAgentBuildError> {
    if let Some(group) = adapter.tools() {
        let Some(tools) = tools.as_mut() else {
            return Err(BaseAgentBuildError::ToolsUnsupported);
        };
        tools.extend_with_group(group)?;
    }
    adapters.push(adapter);
    Ok(())
}

impl<H: ClawHttp> BaseAgent<H> {
    /// Start building an agent over a caller-owned [`TranscriptStore`].
    ///
    /// The transcript store is the only place the filesystem type `F` enters, and
    /// it stays on the *builder*: the built [`BaseAgent`] erases it behind the
    /// [`History`] / [`Transcript`] trait objects. The caller decides how the
    /// store is built and keyed (via [`TranscriptStore::new`]) and may keep a
    /// clone to inspect the conversation without going through `BaseAgent`:
    ///
    /// ```ignore
    /// let store = TranscriptStore::new(agent_id, config, fs);
    /// let view = store.clone();
    /// let agent = BaseAgent::builder(llm, store).build()?;
    /// // later: let messages = view.messages();
    /// ```
    pub fn builder<F: ClawFs + 'static>(
        llm: ClawApi<H>,
        store: TranscriptStore<F>,
    ) -> BaseAgentBuilder<F, H> {
        BaseAgentBuilder {
            llm,
            store,
            tools: None,
            skills: None,
            system_prompt: String::new(),
            inherited_context: Arc::from([]),
            retry_policy: RetryPolicy::default(),
            permission_policy: None,
            agent_id: 0,
            agent_kind: String::new(),
            block_retries: DEFAULT_BLOCK_RETRIES,
            summary_cursor: SummaryCursor::new(),
        }
    }

    /// A read-only view of this agent's conversation transcript, as the narrow
    /// [`History`] capability.
    ///
    /// The concrete conversation-memory type — and its filesystem parameter —
    /// stays hidden behind the trait object, so a reader (CLI inspection, a
    /// test) depends only on this capability, never on storage internals. This
    /// is the same boundary the agent itself lends to its memories.
    pub fn history(&self) -> &dyn History {
        self.transcript.as_history()
    }

    // -- Inbound: the kernel + ergonomic wrappers ---------------------------

    /// Queue a command. The single inbound entry point; everything else wraps it.
    ///
    /// The command is validated against the agent's *projected* state (the state
    /// it will reach once already-queued commands are applied). A valid command
    /// is enqueued for the next [`tick`](Self::tick); an invalid one is rejected
    /// and the agent is left unchanged.
    ///
    /// # Errors
    ///
    /// [`AgentCommandError`] when the command is not legal for the projected
    /// state — e.g. [`Resume`](AgentCommand::Resume) when not paused, or
    /// [`Cancel`](AgentCommand::Cancel) when already idle.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// use claw_core::agent::{AgentCommandError, CancelReason};
    ///
    /// agent.run("summarize the news");            // projected state -> Running
    /// agent.cancel(CancelReason::UserRequested)?; // projected state -> Idle
    ///
    /// // Validated against the *projected* state (the batch so far), before any
    /// // tick runs: resuming the now-idle agent is rejected and the agent is
    /// // left unchanged.
    /// assert!(matches!(agent.resume(), Err(AgentCommandError::CannotResume { .. })));
    /// # Ok::<(), AgentCommandError>(())
    /// ```
    pub fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        let next = classify(self.projected_lifecycle, &command, self.pending_approval)?;
        self.projected_lifecycle = next;
        self.inbox.push_back(Inbound::Command(command));
        Ok(())
    }

    /// Start (or continue) a task with `goal`. Convenience for
    /// [`AppendMessage`](AgentCommand::AppendMessage).
    ///
    /// Infallible: an append is valid in every state (it starts a fresh task when
    /// idle and joins the current one otherwise), so it can never be rejected.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// use claw_core::agent::TickOutcome;
    ///
    /// agent.run("summarize today's news"); // queue the task, then drive with `tick`
    /// assert!(matches!(agent.tick(), TickOutcome::Working | TickOutcome::Yielded { .. }));
    /// ```
    pub fn run(&mut self, goal: impl Into<String>) {
        // `AppendMessage` is accepted in every state; `send_command` cannot reject it.
        let _ = self.send_command(AgentCommand::AppendMessage(goal.into()));
    }

    /// Append a user message. Convenience for
    /// [`AppendMessage`](AgentCommand::AppendMessage). Infallible — see [`run`](Self::run).
    pub fn append_message(&mut self, message: impl Into<String>) {
        // `AppendMessage` is accepted in every state; `send_command` cannot reject it.
        let _ = self.send_command(AgentCommand::AppendMessage(message.into()));
    }

    /// Abandon the current task. Convenience for [`Cancel`](AgentCommand::Cancel).
    ///
    /// # Errors
    ///
    /// [`AgentCommandError::NothingToCancel`] when the agent is idle.
    pub fn cancel(&mut self, reason: CancelReason) -> Result<(), AgentCommandError> {
        self.send_command(AgentCommand::Cancel { reason })
    }

    /// Pause iteration scheduling. Convenience for [`Pause`](AgentCommand::Pause).
    ///
    /// # Errors
    ///
    /// [`AgentCommandError::CannotPause`] unless the agent is running.
    pub fn pause(&mut self) -> Result<(), AgentCommandError> {
        self.send_command(AgentCommand::Pause)
    }

    /// Resume after a pause. Convenience for [`Resume`](AgentCommand::Resume).
    ///
    /// # Errors
    ///
    /// [`AgentCommandError::CannotResume`] unless the agent is paused.
    pub fn resume(&mut self) -> Result<(), AgentCommandError> {
        self.send_command(AgentCommand::Resume)
    }

    /// Resolve a pending approval request. Convenience for
    /// [`ApprovalResult`](AgentCommand::ApprovalResult); pass
    /// [`ApprovalDecision::Approved`] or [`ApprovalDecision::Rejected`].
    ///
    /// # Errors
    ///
    /// [`AgentCommandError::NotAwaitingApproval`] when no approval is pending, or
    /// [`AgentCommandError::ApprovalMismatch`] when `id` is not the pending one.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// use claw_core::agent::{ApprovalDecision, AgentCommandError, TickOutcome};
    ///
    /// // A tick that paused on a permission `Ask` hands back the id.
    /// if let TickOutcome::AwaitingApproval { id, .. } = agent.tick() {
    ///     agent.resolve_approval(id, ApprovalDecision::Approved)?;
    /// }
    /// # Ok::<(), AgentCommandError>(())
    /// ```
    pub fn resolve_approval(
        &mut self,
        id: ApprovalId,
        decision: ApprovalDecision,
    ) -> Result<(), AgentCommandError> {
        self.send_command(AgentCommand::ApprovalResult { id, decision })
    }

    // -- Status -------------------------------------------------------------

    /// The agent's current lifecycle state.
    ///
    /// The primary observability surface: callers branch on this instead of a
    /// grab-bag of `is_*` predicates. See [`AgentState`] for the states.
    pub fn state(&self) -> AgentState {
        self.lifecycle
    }

    /// True while a task is actively iterating (not idle, paused, or awaiting
    /// approval). A thin convenience over [`state`](Self::state).
    pub fn is_running(&self) -> bool {
        self.lifecycle == AgentState::Running
    }

    /// A handle to abort this agent's in-flight iteration from another task. Grab
    /// it before the tick loop starts (see [`AgentAbortHandle`]).
    ///
    /// # Examples
    ///
    /// ```ignore
    /// let handle = agent.abort_handle();      // clone-and-move to another thread
    /// std::thread::spawn(move || handle.abort());
    /// // The next `tick` is preempted at its first checkpoint and returns Working.
    /// ```
    pub fn abort_handle(&self) -> AgentAbortHandle {
        AgentAbortHandle {
            flag: Arc::clone(&self.interruption.flag),
        }
    }

    // -- Context-adapter registration ---------------------------------------

    /// Register a pluggable [`ContextAdapter`]. From now on it pulls this agent's
    /// transcript and contributes context before every iteration; if it provides
    /// any tools, they are merged into the agent's tool set now.
    ///
    /// Call after [`build`](BaseAgentBuilder::build) and before driving the agent.
    /// Adapters contribute in registration order, after the agent's own context
    /// blocks (cross-adapter wire order is fixed by [`BlockKind`], not by this
    /// order).
    ///
    /// # Errors
    ///
    /// - [`BaseAgentBuildError::Tools`] if an adapter tool name clashes with an
    ///   existing tool.
    /// - [`BaseAgentBuildError::ToolsUnsupported`] if the adapter provides tools
    ///   but the configured LLM cannot call tools.
    pub fn register_context_adapter(
        &mut self,
        adapter: Box<dyn ContextAdapter>,
    ) -> Result<(), BaseAgentBuildError> {
        install_context_adapter(&mut self.adapters, &mut self.tools, adapter)
    }

    /// Project every registered adapter's source into this agent's context.
    ///
    /// The returned `Value::Array` is the model's history channel. Blocks and
    /// reminders are applied directly to [`Context`]; history messages are cloned
    /// into the request-local sink and ordered by `claw-context` using
    /// [`BlockKind::sort_key`].
    fn render_adapter_context(&mut self) -> Value {
        let history_view = self.transcript.as_history();
        let tools = self.tools.as_ref();
        let mut sink = self.context.sink();
        for adapter in &mut self.adapters {
            adapter.contribute(
                ContextAdapterInput {
                    history: history_view,
                    tools,
                },
                &mut sink,
            );
        }
        sink.into_history()
    }

    // -- The tick -----------------------------------------------------------

    /// Process queued commands, advance at most one iteration, and report what
    /// happened as a [`TickOutcome`].
    ///
    /// # Examples
    ///
    /// ```ignore
    /// use claw_core::agent::TickOutcome;
    ///
    /// agent.run("summarize today's news");
    /// loop {
    ///     match agent.tick() {
    ///         TickOutcome::Working => continue,            // pump again now
    ///         TickOutcome::Idle => break,                  // nothing to do; await input
    ///         TickOutcome::Yielded { text } => { println!("{text}"); break; }
    ///         TickOutcome::Ended { final_message } => { println!("{final_message}"); break; }
    ///         TickOutcome::Failed(error) => { eprintln!("{error}"); break; }
    ///         _ => break,
    ///     }
    /// }
    /// ```
    pub fn tick(&mut self) -> TickOutcome {
        self.outcome = None;

        // 1. External commands.
        self.drain_inbox();

        // 2. One iteration, if running.
        if self.lifecycle == AgentState::Running {
            let iteration_id = self.next_iteration;
            self.next_iteration = IterationId(iteration_id.0.saturating_add(1));

            // Context assembly is owned by `claw-context`: `run_iteration` calls
            // `Context::request`, which re-renders the prefix only if a block
            // changed since last tick. Nothing to prepare or degrade here.
            let outcome = self.run_iteration(iteration_id);
            self.reduce_outcome(outcome);
            // 3. Internal-tool signals raised during the iteration, folded
            //    back through the same reducer.
            self.drain_control_signals();
            self.drain_inbox();
        }

        // The inbox is now drained; realign the projection with the committed
        // state so the next batch of commands is validated against the truth.
        self.projected_lifecycle = self.lifecycle;

        self.outcome.take().unwrap_or_else(|| match self.lifecycle {
            AgentState::Running => TickOutcome::Working,
            _ => TickOutcome::Idle,
        })
    }

    /// Run exactly one [`IterationLoop`] round over current context.
    ///
    /// [`Context`] is the single assembler: [`Context::request`] pairs the cached
    /// system prefix with the message tail's two segments — the persisted history
    /// snapshot (a cheap `Arc` clone) and the ephemeral reminder tail — into one
    /// `RequestContext`, re-rendering the prefix only on a real change. No request
    /// stitching happens anywhere else; reminders are never written to memory, so
    /// the cached system/history prefix is untouched.
    fn run_iteration(&mut self, iteration_id: IterationId) -> IterationResult {
        // Pull each adapter's blocks into the private context and assemble the
        // model's history channel from the adapter `messages()` contributions.
        let history = self.render_adapter_context();
        // Take the disjoint field borrows first, then borrow `context` mutably for
        // the (lazily rebuilt) request. `request` takes `&mut self.context`, so the
        // permission gate must be derived from `self.gate` directly here rather
        // than through a whole-`&self` helper.
        let iteration_loop = IterationLoop {
            llm: &mut self.llm,
            interruption: &self.interruption,
            retry: self.retry_policy,
        };
        let tools = self.tools.as_ref();
        let gate = self.gate.as_ref().map(|gate| gate as &dyn ToolGate);
        let context = self.context.request(&history);
        let step = IterationStep {
            iteration_id,
            system_prompt: SystemPrompt(context.system()),
            messages: ChatMessages(context.history()),
            reminders: context.reminders(),
            tools,
            gate,
        };
        iteration_loop.run(step)
    }

    // -- Reducer: the single state-mutation funnel for inbound --------------

    fn drain_inbox(&mut self) {
        while let Some(inbound) = self.inbox.pop_front() {
            self.apply_inbound(inbound);
        }
    }

    /// Move internal-tool signals onto the inbox so they reduce like commands.
    fn drain_control_signals(&mut self) {
        let signals: Vec<ControlSignal> = {
            let mut sink = self
                .control
                .lock()
                .unwrap_or_else(|poison| poison.into_inner());
            sink.drain(..).collect()
        };
        for signal in signals {
            self.inbox.push_back(Inbound::Control(signal));
        }
    }

    /// THE reducer: the only place inbound input mutates agent state.
    ///
    /// External [`AgentCommand`]s arrive here already validated by
    /// [`classify`](Self::classify) (at [`send_command`](Self::send_command) time),
    /// so the state transitions below are unconditional — an illegal command never
    /// reaches the inbox. Internal [`ControlSignal`]s are agent-generated and
    /// always legal in the state that raised them.
    fn apply_inbound(&mut self, inbound: Inbound) {
        match inbound {
            Inbound::Command(AgentCommand::AppendMessage(text)) => {
                let starts_task = self.lifecycle == AgentState::Idle;
                if starts_task {
                    // A fresh task: reset the iteration counter.
                    self.next_iteration = IterationId(0);
                    self.lifecycle = AgentState::Running;
                }
                // Write the user turn directly; `starts_task` lets the transcript
                // flush any turn a prior task left open before opening a new one.
                self.transcript.append_user(&text, starts_task);
            }
            Inbound::Command(AgentCommand::Cancel { reason }) => {
                // Cancel is *disruptive* (unlike pause/resume/append/approve, which
                // are normal flow): record why the task was abandoned so the next
                // task does not inherit an unexplained, half-finished exchange. The
                // transcript writes the marker (and flushes any open turn).
                self.transcript.commit_cancellation(cancel_marker(&reason));
                self.pending_approval = None;
                self.pending_grant_signatures.clear();
                self.lifecycle = AgentState::Idle;
                self.outcome = Some(TickOutcome::Cancelled { reason });
            }
            Inbound::Command(AgentCommand::Pause) => {
                self.lifecycle = AgentState::Paused;
            }
            Inbound::Command(AgentCommand::Resume) => {
                self.lifecycle = AgentState::Running;
            }
            Inbound::Command(AgentCommand::ApprovalResult { id, decision }) => {
                // The verdict re-enters the transcript as a synthetic user turn.
                let marker = approval_marker(id, &decision);
                self.transcript.append_user(&marker, false);
                // Record the decision against the asked-about actions so the
                // retried tool calls resolve without asking again.
                self.record_grants(&decision);
                self.pending_approval = None;
                self.lifecycle = AgentState::Running;
            }
            Inbound::Control(ControlSignal::EndConversation { final_message }) => {
                self.transcript.commit_ended(&final_message);
                self.lifecycle = AgentState::Idle;
                self.outcome = Some(TickOutcome::Ended { final_message });
            }
        }
    }

    /// Reduce one iteration outcome into the tick's outcome and lifecycle. The
    /// second funnel (the first being [`apply_inbound`](Self::apply_inbound)) — its
    /// input is the LLM/tool round result, not a command.
    fn reduce_outcome(&mut self, outcome: IterationResult) {
        match outcome {
            Ok(IterationOutcome::Completed(CompletedOutcome { kind, .. })) => match kind {
                CompletedKind::PlainText(answer) => {
                    // Commit the answer directly (closing the turn).
                    self.transcript
                        .commit_assistant(&answer.text, answer.raw_message_json.as_deref());
                    // Non-terminal: hand back to the caller, go idle for next input.
                    self.lifecycle = AgentState::Idle;
                    self.outcome = Some(TickOutcome::Yielded { text: answer.text });
                }
                CompletedKind::Tools(tools) => {
                    // A tool round: merge the messages and keep working. The
                    // per-tool summary (`tools.runs`) stays internal — base_agent
                    // does not surface it as an outcome. The patch is well-formed
                    // even for gating-blocked / permission-refused calls (each got
                    // a matched tool error), so committing never leaves a dangling
                    // call. Write the patch directly.
                    self.transcript.commit_patch(&tools.appended.0);
                    self.apply_tool_block_policy(&tools.runs);
                    // A permission `Ask` pauses the agent for a human decision
                    // (unless the round already failed the task via the block
                    // policy above, which leaves `self.outcome` set).
                    self.maybe_raise_approval(&tools.runs);
                }
            },
            Ok(IterationOutcome::Preempted(outcome)) => {
                // A preempted iteration is terminal; the task is not. Merge only a
                // well-formed partial patch, then re-iterate next tick.
                self.merge_preempt_patch(outcome);
            }
            Err(error) => self.fail_with(error.into()),
        }
    }

    /// Apply the soft-hide "retry then fail" policy after a tool round.
    ///
    /// The streak counting and budget live in the agent's [`BlockPolicy`] (the
    /// model already received a tool error to self-correct from for each blocked
    /// call). The agent turns an [`Exhausted`](ToolBlockVerdict::Exhausted) verdict
    /// into a task failure — the policy has no concept of a task.
    fn apply_tool_block_policy(&mut self, runs: &[ToolRun]) {
        let blocked: Vec<&str> = runs
            .iter()
            .filter(|run| run.blocked)
            .map(|run| run.name.as_str())
            .collect();
        if !blocked.is_empty() {
            tracing::warn!(tools = ?blocked, "tool gate blocked");
        }
        if let ToolBlockVerdict::Exhausted { name } = self.block_policy.record_round(&blocked) {
            self.fail_with(AgentRunError::ToolNotPermitted { name });
        }
    }

    /// Pause for a human decision when the permission policy asked about any call
    /// this round. No-op if the round already produced an outcome (e.g. the block
    /// policy failed the task) or no call needs approval.
    ///
    /// The asked-about action signatures are remembered so the
    /// [`ApprovalResult`](AgentCommand::ApprovalResult) can grant/deny them; the
    /// approver sees the first call's reason as the summary.
    fn maybe_raise_approval(&mut self, runs: &[ToolRun]) {
        if self.outcome.is_some() {
            return;
        }
        let pending: Vec<(String, String)> = runs
            .iter()
            .filter_map(|run| {
                run.approval
                    .as_ref()
                    .map(|approval| (approval.summary.clone(), approval.signature.clone()))
            })
            .collect();
        let Some((summary, _)) = pending.first().cloned() else {
            return;
        };
        self.pending_grant_signatures = pending.into_iter().map(|(_, sig)| sig).collect();
        let id = self.allocate_approval_id();
        self.pending_approval = Some(id);
        self.lifecycle = AgentState::AwaitingApproval;
        self.outcome = Some(TickOutcome::AwaitingApproval { id, summary });
    }

    /// Record a human decision against the actions that were asked about, so the
    /// retried calls resolve directly. No-op without a permission gate.
    ///
    /// The agent's [`ApprovalDecision`] is mapped to the permission-layer
    /// [`Grant`] the gate stores (`claw-tool`/`claw-permission` know nothing of
    /// the agent's command vocabulary).
    fn record_grants(&mut self, decision: &ApprovalDecision) {
        let signatures = std::mem::take(&mut self.pending_grant_signatures);
        if let Some(gate) = self.gate.as_mut() {
            let grant = match decision {
                ApprovalDecision::Approved => Grant::Granted,
                ApprovalDecision::Rejected(reason) => Grant::Denied(reason.clone()),
            };
            gate.record_decision(&signatures, &grant);
        }
    }

    /// End the task with a failure outcome, leaving the agent idle and reusable.
    fn fail_with(&mut self, error: AgentRunError) {
        tracing::warn!(%error, "base_agent task failed");
        self.lifecycle = AgentState::Idle;
        self.outcome = Some(TickOutcome::Failed(error));
    }

    // -- Memory helpers -----------------------------------------------------

    /// Merge a preemption's partial patch only when it is well-formed.
    ///
    /// A mid-tool-round preempt can leave an assistant message whose `tool_calls`
    /// have no matching tool results — committing that would make the next LLM
    /// call ill-formed. So such a patch is dropped (the half-done work simply did
    /// not happen); a clean patch is emitted for the conversation memory to write.
    fn merge_preempt_patch(&mut self, outcome: PreemptedOutcome) {
        let Some(produced) = outcome.produced else {
            return;
        };
        if has_dangling_tool_calls(&produced.0) {
            tracing::info!("dropping preempted partial patch: unmatched tool_calls");
            return;
        }
        self.transcript.commit_patch(&produced.0);
    }

    fn allocate_approval_id(&mut self) -> ApprovalId {
        let id = ApprovalId(self.next_approval);
        self.next_approval = self.next_approval.saturating_add(1);
        id
    }
}

/// The interruption marker recorded for a cancelled task, keyed on the
/// [`CancelReason`]. Written via [`Transcript::commit_cancellation`] as the
/// abandoned turn's closing note.
fn cancel_marker(reason: &CancelReason) -> &'static str {
    match reason {
        CancelReason::UserRequested => "[conversation interrupted: cancelled by the user]",
        CancelReason::Superseded => "[conversation interrupted: superseded by a new task]",
        CancelReason::Shutdown => "[conversation interrupted: the agent is shutting down]",
    }
}

/// The synthetic user-turn text recording a human approval decision, so the
/// retried iteration sees the verdict. Written via [`Transcript::append_user`].
fn approval_marker(id: ApprovalId, decision: &ApprovalDecision) -> String {
    match decision {
        ApprovalDecision::Approved => format!("[approval {id}] approved by the human."),
        ApprovalDecision::Rejected(reason) => {
            format!("[approval {id}] rejected by the human: {reason}")
        }
    }
}

/// True when `patch` contains an assistant `tool_calls` id with no matching
/// `tool` message (`tool_call_id`).
fn has_dangling_tool_calls(patch: &Value) -> bool {
    let Some(items) = patch.as_array() else {
        return false;
    };
    let mut expected: Vec<&str> = Vec::new();
    let mut satisfied: HashSet<&str> = HashSet::new();
    for message in items {
        if let Some(calls) = message.get("tool_calls").and_then(Value::as_array) {
            for call in calls {
                if let Some(id) = call.get("id").and_then(Value::as_str) {
                    expected.push(id);
                }
            }
        }
        if let Some(id) = message.get("tool_call_id").and_then(Value::as_str) {
            satisfied.insert(id);
        }
    }
    expected.iter().any(|id| !satisfied.contains(id))
}

// ===========================================================================
// Builder
// ===========================================================================

/// Builder for a [`BaseAgent`], collecting all construction-time configuration.
///
/// Separates the *build* phase from the *run* phase: system prompt, tools, and
/// skills are set here (optional, any order), and [`build`](Self::build) produces
/// a finished agent that exposes only the runtime command/tick API.
#[must_use = "a BaseAgentBuilder does nothing until `.build()` is called"]
pub struct BaseAgentBuilder<F: ClawFs + 'static, H: ClawHttp> {
    llm: ClawApi<H>,
    store: TranscriptStore<F>,
    tools: Option<ToolSet>,
    skills: Option<SkillSet>,
    system_prompt: String,
    inherited_context: Arc<[Block<'static>]>,
    retry_policy: RetryPolicy,
    permission_policy: Option<Arc<dyn PermissionPolicy>>,
    agent_id: u64,
    agent_kind: String,
    block_retries: u32,
    /// The boundary the built-in recent-tail adapter renders past. Defaults to a
    /// fresh cursor (nothing summarized → whole transcript verbatim); the agent
    /// layer overrides it with [`with_summary_cursor`](Self::with_summary_cursor)
    /// and hands the same cursor to its rolling-summary adapter.
    summary_cursor: SummaryCursor,
}

impl<F: ClawFs + 'static, H: ClawHttp> BaseAgentBuilder<F, H> {
    /// Set the tools available to the agent across all tasks.
    ///
    /// Takes a pre-built [`ToolSet`]; the agent's built-in control tool
    /// (`end_conversation`) is merged on at [`build`](Self::build).
    pub fn with_tools(mut self, tools: ToolSet) -> Self {
        self.tools = Some(tools);
        self
    }

    /// Set how many consecutive soft-hide-blocked tool rounds to tolerate before
    /// the agent fails the task with
    /// [`ToolNotPermitted`](AgentRunError::ToolNotPermitted). Each tolerated round
    /// is one self-correction nudge to the model; `0` fails on the first blocked
    /// round. Defaults to [`DEFAULT_BLOCK_RETRIES`](claw_tool::DEFAULT_BLOCK_RETRIES).
    pub fn with_block_retries(mut self, retries: u32) -> Self {
        self.block_retries = retries;
        self
    }

    /// Share the [`SummaryCursor`] the built-in recent-tail adapter renders past.
    ///
    /// The recent-tail adapter renders the committed turns *after* this cursor
    /// (plus the open turn). Hand the **same** cursor to the agent layer's
    /// rolling-summary adapter (which advances it as it summarizes) so the two
    /// halves tile the transcript with no gap or overlap. Without this, the
    /// default cursor stays at 0 and the whole transcript renders verbatim — the
    /// correct standalone behavior when no summary adapter is attached.
    pub fn with_summary_cursor(mut self, cursor: SummaryCursor) -> Self {
        self.summary_cursor = cursor;
        self
    }

    /// Set the agent's skills. The [`SkillSet`] is handed to the skill context
    /// adapter, which projects loaded skills as `ActiveSkills` and owns the
    /// optional skill-management tools for tool-capable backends.
    pub fn with_skills(mut self, skills: SkillSet) -> Self {
        self.skills = Some(skills);
        self
    }

    /// Set the agent's system prompt — its instructions/persona, fixed across all
    /// of its tasks. Defaults to empty.
    pub fn with_system_prompt(mut self, system_prompt: impl Into<String>) -> Self {
        self.system_prompt = system_prompt.into();
        self
    }

    /// Inject scope-layered prose blocks from above (Global, then Session) that
    /// render ahead of the agent's own blocks in the assembled system prefix.
    ///
    /// Shared as an `Arc<[Block]>` so several agents can reference one computed
    /// set for byte-identical prefixes. Defaults to empty — the standalone-agent
    /// behavior, where only the agent's own blocks are assembled.
    pub fn with_inherited_context(mut self, blocks: Arc<[Block<'static>]>) -> Self {
        self.inherited_context = blocks;
        self
    }

    /// Override the [`RetryPolicy`] applied to every per-iteration LLM call.
    ///
    /// Defaults to [`RetryPolicy::default`] (2 retries on transient transport
    /// failures). Pass [`RetryPolicy::none`] to fail fast on the first error
    /// (e.g. to make a single transport error surface as
    /// [`TickOutcome::Failed`] without burning the retry budget).
    ///
    /// # Examples
    ///
    /// ```ignore
    /// use claw_core::agent::{BaseAgent, RetryPolicy};
    ///
    /// let agent = BaseAgent::builder(llm, memory)
    ///     .with_retry_policy(RetryPolicy::none()) // no retry: first transport error -> Failed
    ///     .build()?;
    /// # Ok::<(), claw_core::agent::BaseAgentBuildError>(())
    /// ```
    pub fn with_retry_policy(mut self, retry_policy: RetryPolicy) -> Self {
        self.retry_policy = retry_policy;
        self
    }

    /// Install a permission policy that gates every tool call. Each classified
    /// call is evaluated to `Allow` / `Ask` / `Deny`; `Ask` pauses the agent for a
    /// human decision (reusing the approval flow), and the decision is remembered
    /// so the retried call resolves directly.
    ///
    /// Without this, the agent has no permission layer: every call that passes
    /// soft-hide gating runs. Pair with [`with_identity`](Self::with_identity) so
    /// the policy sees the acting agent.
    pub fn with_permission_policy(mut self, policy: Arc<dyn PermissionPolicy>) -> Self {
        self.permission_policy = Some(policy);
        self
    }

    /// Set the acting agent's identity (numeric id + kind), passed to the
    /// permission policy on each evaluation. Defaults to `(0, "")`; only relevant
    /// when a [permission policy](Self::with_permission_policy) is installed.
    pub fn with_identity(mut self, agent_id: u64, agent_kind: impl Into<String>) -> Self {
        self.agent_id = agent_id;
        self.agent_kind = agent_kind.into();
        self
    }

    /// Finish configuration and produce a runnable [`BaseAgent`].
    ///
    /// The built-in tool group is merged onto the caller's tools when the LLM
    /// supports tool calls. A configured [`SkillSet`] is wrapped by the skill
    /// context adapter, which contributes `ActiveSkills` and, for tool-capable
    /// backends, provides the skill-management tool group.
    ///
    /// # Errors
    ///
    /// - [`BaseAgentBuildError::Tools`] if a built-in tool name clashes with a
    ///   caller tool.
    /// - [`BaseAgentBuildError::ToolsUnsupported`] if tools were provided but the
    ///   configured LLM cannot call tools.
    pub fn build(self) -> Result<BaseAgent<H>, BaseAgentBuildError> {
        let control: ControlSink = Arc::new(Mutex::new(VecDeque::new()));
        let supports_tools = self.llm.profile().supports_tools();

        let mut tools = if supports_tools {
            let mut tools = self.tools.unwrap_or_else(ToolSet::empty);
            tools.extend_with_group(internal_tool_group(Arc::clone(&control)))?;
            Some(tools)
        } else {
            if self.tools.is_some() {
                return Err(BaseAgentBuildError::ToolsUnsupported);
            }
            None
        };

        let gate = self
            .permission_policy
            .map(|policy| PermissionGate::new(policy, self.agent_id, self.agent_kind));

        // Declare the construction-time blocks the context owns up front: the
        // inherited (Global/Session) blocks and the agent instruction. Tool policy
        // is projected by its adapter from the current ToolSet, so tool mutations
        // never need to manually sync prompt prose here.
        let mut context = Context::new();
        for block in self.inherited_context.iter() {
            context.with(block.clone());
        }
        context.with(Block::new(BlockKind::AgentInstruction, self.system_prompt));

        // The conversation transcript drives the model's history off one store.
        // The agent writes verbatim turns through the `Transcript` owner; the
        // history is then assembled (in `render_adapter_context`) from context
        // adapters reading clones of the same `Arc`-backed store. The verbatim
        // recent tail (`RecentContext`) is intrinsic — it needs only the store and
        // the summary cursor — so it is registered here up front, ahead of any
        // caller adapter, giving every agent its own history with no extra wiring.
        // The rolling summary (`ConversationSummary`) carries compaction *policy*
        // (pool + compactor + budgets) the builder does not have, so the agent
        // layer (`GenericAgent::new`) registers it and shares the same
        // `summary_cursor`. This is the only place `F` is erased — both the trait
        // object and the boxed adapter drop it, so the built agent is
        // filesystem-agnostic.
        let tool_policy: Box<dyn ContextAdapter> = Box::new(ToolPolicyContextAdapter::new());
        let skill_adapter = self.skills.map(|skills| {
            let tool_mode = if supports_tools {
                SkillTools::Enabled
            } else {
                SkillTools::Disabled
            };
            Box::new(SkillContextAdapter::new(skills, tool_mode)) as Box<dyn ContextAdapter>
        });
        let recent: Box<dyn ContextAdapter> = Box::new(RecentMessagesContextAdapter::new(
            self.store.clone(),
            self.summary_cursor,
        ));
        let transcript: Arc<dyn Transcript> = Arc::new(self.store);
        let mut adapters = Vec::new();
        install_context_adapter(&mut adapters, &mut tools, tool_policy)?;
        if let Some(skill_adapter) = skill_adapter {
            install_context_adapter(&mut adapters, &mut tools, skill_adapter)?;
        }
        install_context_adapter(&mut adapters, &mut tools, recent)?;

        Ok(BaseAgent {
            llm: self.llm,
            retry_policy: self.retry_policy,
            interruption: AgentInterruption {
                flag: Arc::new(AtomicBool::new(false)),
            },
            transcript,
            tools,
            block_policy: BlockPolicy::new(self.block_retries),
            context,
            gate,
            pending_grant_signatures: Vec::new(),
            next_iteration: IterationId(0),
            next_approval: 0,
            pending_approval: None,
            lifecycle: AgentState::Idle,
            projected_lifecycle: AgentState::Idle,
            outcome: None,
            control,
            adapters,
            inbox: VecDeque::new(),
        })
    }
}

// ===========================================================================
// Tests: the FSM transition table
// ===========================================================================

/// The FSM transition table: the single authority on whether `command` is legal
/// in `state`, and what state it leads to. A free function (no `&self`, and
/// independent of the memory backend `F`) so it is trivially testable and is the
/// one place command validity is decided — [`BaseAgent::apply_inbound`] trusts
/// its verdict.
///
/// `pending_approval` is the id the agent is currently waiting on; it is only
/// consulted in the [`AwaitingApproval`](AgentState::AwaitingApproval) state.
/// The match is exhaustive over every `(state, command)` pair so a new state or
/// command cannot be silently mishandled.
fn classify(
    state: AgentState,
    command: &AgentCommand,
    pending_approval: Option<ApprovalId>,
) -> Result<AgentState, AgentCommandError> {
    use AgentCommand as Command;
    use AgentState as State;
    match (state, command) {
        // AppendMessage is accepted in every state: from idle it starts a
        // fresh task (-> Running); otherwise it joins without changing state.
        (State::Idle, Command::AppendMessage(_)) => Ok(State::Running),
        (State::Running, Command::AppendMessage(_)) => Ok(State::Running),
        (State::Paused, Command::AppendMessage(_)) => Ok(State::Paused),
        (State::AwaitingApproval, Command::AppendMessage(_)) => Ok(State::AwaitingApproval),

        // Cancel ends an active task; there is nothing to cancel when idle.
        (State::Idle, Command::Cancel { .. }) => Err(AgentCommandError::NothingToCancel),
        (State::Running | State::Paused | State::AwaitingApproval, Command::Cancel { .. }) => {
            Ok(State::Idle)
        }

        // Pause only makes sense while actively running.
        (State::Running, Command::Pause) => Ok(State::Paused),
        (state @ (State::Idle | State::Paused | State::AwaitingApproval), Command::Pause) => {
            Err(AgentCommandError::CannotPause { state })
        }

        // Resume only from a paused task.
        (State::Paused, Command::Resume) => Ok(State::Running),
        (state @ (State::Idle | State::Running | State::AwaitingApproval), Command::Resume) => {
            Err(AgentCommandError::CannotResume { state })
        }

        // An approval result needs a matching pending request.
        (State::AwaitingApproval, Command::ApprovalResult { id, .. }) => match pending_approval {
            Some(pending) if pending == *id => Ok(State::Running),
            Some(pending) => Err(AgentCommandError::ApprovalMismatch {
                expected: pending,
                got: *id,
            }),
            // AwaitingApproval always carries a pending id; a missing one is
            // an impossible invariant, surfaced rather than silently accepted.
            None => Err(AgentCommandError::NotAwaitingApproval { state }),
        },
        (
            state @ (State::Idle | State::Running | State::Paused),
            Command::ApprovalResult { .. },
        ) => Err(AgentCommandError::NotAwaitingApproval { state }),
    }
}

#[cfg(test)]
mod transition_tests {
    use super::*;

    fn append() -> AgentCommand {
        AgentCommand::AppendMessage("hi".into())
    }

    fn cancel() -> AgentCommand {
        AgentCommand::Cancel {
            reason: CancelReason::UserRequested,
        }
    }

    fn approval(id: usize) -> AgentCommand {
        AgentCommand::ApprovalResult {
            id: ApprovalId(id),
            decision: ApprovalDecision::Approved,
        }
    }

    fn classify(
        state: AgentState,
        command: &AgentCommand,
    ) -> Result<AgentState, AgentCommandError> {
        super::classify(state, command, None)
    }

    #[test]
    fn append_is_accepted_in_every_state() {
        assert_eq!(
            classify(AgentState::Idle, &append()),
            Ok(AgentState::Running)
        );
        assert_eq!(
            classify(AgentState::Running, &append()),
            Ok(AgentState::Running)
        );
        assert_eq!(
            classify(AgentState::Paused, &append()),
            Ok(AgentState::Paused)
        );
        assert_eq!(
            classify(AgentState::AwaitingApproval, &append()),
            Ok(AgentState::AwaitingApproval)
        );
    }

    #[test]
    fn cancel_ends_a_task_but_not_when_idle() {
        assert_eq!(
            classify(AgentState::Running, &cancel()),
            Ok(AgentState::Idle)
        );
        assert_eq!(
            classify(AgentState::Paused, &cancel()),
            Ok(AgentState::Idle)
        );
        assert_eq!(
            classify(AgentState::AwaitingApproval, &cancel()),
            Ok(AgentState::Idle)
        );
        assert_eq!(
            classify(AgentState::Idle, &cancel()),
            Err(AgentCommandError::NothingToCancel)
        );
    }

    #[test]
    fn pause_only_from_running() {
        assert_eq!(
            classify(AgentState::Running, &AgentCommand::Pause),
            Ok(AgentState::Paused)
        );
        for state in [
            AgentState::Idle,
            AgentState::Paused,
            AgentState::AwaitingApproval,
        ] {
            assert_eq!(
                classify(state, &AgentCommand::Pause),
                Err(AgentCommandError::CannotPause { state })
            );
        }
    }

    #[test]
    fn resume_only_from_paused() {
        assert_eq!(
            classify(AgentState::Paused, &AgentCommand::Resume),
            Ok(AgentState::Running)
        );
        for state in [
            AgentState::Idle,
            AgentState::Running,
            AgentState::AwaitingApproval,
        ] {
            assert_eq!(
                classify(state, &AgentCommand::Resume),
                Err(AgentCommandError::CannotResume { state })
            );
        }
    }

    /// The motivating case: cancel leaves the agent idle, so a following resume is
    /// rejected instead of being silently dropped.
    #[test]
    fn cancel_then_resume_is_rejected() {
        let after_cancel = classify(AgentState::Running, &cancel()).expect("cancel from running");
        assert_eq!(after_cancel, AgentState::Idle);
        assert_eq!(
            classify(after_cancel, &AgentCommand::Resume),
            Err(AgentCommandError::CannotResume {
                state: AgentState::Idle
            })
        );
    }

    #[test]
    fn approval_requires_awaiting_and_matching_id() {
        // Not awaiting in any other state.
        for state in [AgentState::Idle, AgentState::Running, AgentState::Paused] {
            assert_eq!(
                super::classify(state, &approval(1), None),
                Err(AgentCommandError::NotAwaitingApproval { state })
            );
        }
        // Matching id resumes; a mismatch is reported with both ids.
        assert_eq!(
            super::classify(
                AgentState::AwaitingApproval,
                &approval(7),
                Some(ApprovalId(7))
            ),
            Ok(AgentState::Running)
        );
        assert_eq!(
            super::classify(
                AgentState::AwaitingApproval,
                &approval(7),
                Some(ApprovalId(9))
            ),
            Err(AgentCommandError::ApprovalMismatch {
                expected: ApprovalId(9),
                got: ApprovalId(7),
            })
        );
    }
}

// ===========================================================================
// Tests: soft-hide tool gating (drives the pub(crate) gating hooks, so it must
// live in-crate; a small self-contained harness keeps it hermetic)
// ===========================================================================

#[cfg(test)]
mod gating_tests {
    #![allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]

    use std::sync::Arc;

    use claw_api::{BackendKind, ClawApi, ClawApiConfig};
    use claw_interface::{CapturingHttp, ClawHttp, MemFs, ScriptedHttp};
    use claw_memory::{TranscriptConfig, TranscriptStore};
    use serde_json::{json, Value};

    use crate::agent::{AgentId, AgentRunError, BaseAgent, BaseAgentBuilder, TickOutcome};
    use claw_tool::{
        AllowedTools, Tool, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolSet,
    };

    // HTTP doubles (ScriptedHttp / CapturingHttp, httpmock feature) are shared from
    // claw_interface.

    // Test tools ----------------------------------------------------------------

    /// Echoes its arguments back; used as the "allowed" tool.
    struct EchoTool;

    impl ToolHandler for EchoTool {
        fn name(&self) -> &str {
            "echo"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"echo","description":"Echo"}}"#
        }
        fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: format!("echo:{}", call.arguments_json),
                ok: true,
            })
        }
    }

    /// Writes something; used as the "disallowed" tool.
    struct WriterTool;

    impl ToolHandler for WriterTool {
        fn name(&self) -> &str {
            "writer"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"writer","description":"Write"}}"#
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: "wrote".into(),
                ok: true,
            })
        }
    }

    struct UsageTool;

    impl ToolHandler for UsageTool {
        fn name(&self) -> &str {
            "usage_tool"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"usage_tool","description":"Usage"}}"#
        }
        fn usage(&self) -> Option<&str> {
            Some("Use usage_tool only when checking tool policy projection.")
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: "used".into(),
                ok: true,
            })
        }
    }

    fn caller_tools() -> ToolSet {
        ToolSet::new([Tool::new(EchoTool), Tool::new(WriterTool)]).expect("tool set")
    }

    // Builders / drivers --------------------------------------------------------

    fn build_llm<H: ClawHttp>(http: H) -> ClawApi<H> {
        let config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        );
        ClawApi::init(config, http).expect("init llm")
    }

    fn scripted_llm(bodies: Vec<String>) -> ClawApi<ScriptedHttp> {
        build_llm(ScriptedHttp::new(bodies))
    }

    fn test_store(agent_id: AgentId) -> TranscriptStore<MemFs> {
        TranscriptStore::new(
            agent_id.0,
            TranscriptConfig::new(format!("/mem/agent-{}", agent_id.0)),
            MemFs::default(),
        )
    }

    /// A builder plus a cloned read-only view of the same store.
    fn builder_with_view<H: ClawHttp>(
        llm: ClawApi<H>,
        agent_id: AgentId,
    ) -> (BaseAgentBuilder<MemFs, H>, TranscriptStore<MemFs>) {
        let store = test_store(agent_id);
        let view = store.clone();
        (BaseAgent::builder(llm, store), view)
    }

    fn body_plain_text(text: &str) -> String {
        json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
    }

    fn body_tool_call(id: &str, name: &str, arguments_json: &str) -> String {
        json!({
            "choices": [{
                "message": {
                    "role": "assistant",
                    "tool_calls": [{
                        "id": id,
                        "function": { "name": name, "arguments": arguments_json }
                    }]
                }
            }]
        })
        .to_string()
    }

    fn body_end_conversation(final_message: &str) -> String {
        body_tool_call(
            "e1",
            "end_conversation",
            &json!({ "final_message": final_message }).to_string(),
        )
    }

    fn run_to_completion<H: ClawHttp>(agent: &mut BaseAgent<H>) -> String {
        loop {
            match agent.tick() {
                TickOutcome::Working => continue,
                TickOutcome::Yielded { text } => return text,
                TickOutcome::Ended { final_message } => return final_message,
                TickOutcome::Failed(error) => panic!("unexpected agent failure: {error}"),
                other => panic!("unexpected outcome: {other:?}"),
            }
        }
    }

    fn transcript_contents(view: &TranscriptStore<MemFs>) -> Vec<String> {
        view.messages()
            .as_array()
            .map(|items| {
                items
                    .iter()
                    .filter_map(|m| m.get("content").and_then(Value::as_str))
                    .map(str::to_string)
                    .collect()
            })
            .unwrap_or_default()
    }

    fn first_tool_message(messages: &Value) -> Option<Value> {
        messages
            .as_array()?
            .iter()
            .find(|m| m.get("role").and_then(Value::as_str) == Some("tool"))
            .cloned()
    }

    /// True when every assistant `tool_calls[].id` has a matching `tool` message.
    fn no_dangling_tool_calls(messages: &Value) -> bool {
        let Some(items) = messages.as_array() else {
            return false;
        };
        let mut expected: Vec<String> = Vec::new();
        let mut satisfied: Vec<String> = Vec::new();
        for message in items {
            if let Some(calls) = message.get("tool_calls").and_then(Value::as_array) {
                for call in calls {
                    if let Some(id) = call.get("id").and_then(Value::as_str) {
                        expected.push(id.to_string());
                    }
                }
            }
            if let Some(id) = message.get("tool_call_id").and_then(Value::as_str) {
                satisfied.push(id.to_string());
            }
        }
        expected.iter().all(|id| satisfied.contains(id))
    }

    // Tests ---------------------------------------------------------------------

    /// A disallowed tool call is refused with a *matched* tool error (no dangling
    /// call) and, within the retry budget, the agent keeps working and self-corrects.
    #[test]
    fn disallowed_tool_is_refused_with_matched_error() {
        let (builder, view) = builder_with_view(
            scripted_llm(vec![
                body_tool_call("t1", "writer", "{}"),
                body_end_conversation("done"),
            ]),
            AgentId(1),
        );
        let tools = caller_tools().with_active_tools(AllowedTools::new(["end_conversation"]));
        let mut agent = builder.with_tools(tools).build().expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "done");

        let messages = view.messages();
        assert!(
            no_dangling_tool_calls(&messages),
            "blocked call left dangling"
        );
        let tool_message = first_tool_message(&messages).expect("a tool message was committed");
        assert_eq!(tool_message["tool_call_id"], "t1");
        assert_eq!(tool_message["is_error"], true);
        let content = tool_message["content"].as_str().unwrap_or_default();
        assert!(
            content.contains("not available in the current phase"),
            "unexpected blocked-tool content: {content}"
        );
    }

    /// With the default budget (1), a second *consecutive* blocked round fails the
    /// task with `ToolNotPermitted` naming the refused tool.
    #[test]
    fn two_consecutive_blocks_fail_the_task() {
        let (builder, _view) = builder_with_view(
            scripted_llm(vec![
                body_tool_call("t1", "writer", "{}"),
                body_tool_call("t2", "writer", "{}"),
            ]),
            AgentId(1),
        );
        let tools = caller_tools().with_active_tools(AllowedTools::new(["end_conversation"]));
        let mut agent = builder.with_tools(tools).build().expect("build");

        agent.run("go");
        // First block: nudged, still working.
        assert!(matches!(agent.tick(), TickOutcome::Working));
        // Second consecutive block: budget exhausted -> failed.
        match agent.tick() {
            TickOutcome::Failed(AgentRunError::ToolNotPermitted { name }) => {
                assert_eq!(name, "writer")
            }
            other => panic!("expected ToolNotPermitted, got {other:?}"),
        }
        // Failed leaves the agent idle and reusable.
        assert!(!agent.is_running());
    }

    /// A clean tool round between two blocks resets the counter, so the budget of 1
    /// is never exceeded and the task completes.
    #[test]
    fn clean_round_resets_block_counter() {
        let (builder, _view) = builder_with_view(
            scripted_llm(vec![
                body_tool_call("t1", "writer", "{}"), // block (count 1)
                body_tool_call("t2", "echo", "{}"),   // clean (reset to 0)
                body_tool_call("t3", "writer", "{}"), // block (count 1 again)
                body_end_conversation("done"),
            ]),
            AgentId(1),
        );
        // echo is permitted (the clean round); writer is not.
        let tools =
            caller_tools().with_active_tools(AllowedTools::new(["echo", "end_conversation"]));
        let mut agent = builder.with_tools(tools).build().expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "done");
    }

    /// A model-issued `load_skill` tool call lands the skill's body in the
    /// system prompt of the *next* LLM request.
    #[test]
    fn load_skill_tool_injects_skill_body_into_context() {
        use crate::memory::skill_test_support::skill_registry;
        use claw_skill::SkillSet;

        let http = CapturingHttp::new(vec![
            body_tool_call("s1", "load_skill", r#"{"skill":"alpha"}"#),
            body_plain_text("done"),
        ]);
        let (builder, _view) = builder_with_view(build_llm(http.clone()), AgentId(1));
        let registry = skill_registry(&[("alpha", "Alpha skill")]);
        let mut agent = builder
            .with_skills(SkillSet::new(registry))
            .build()
            .expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "done");

        let bodies = http.captured_bodies();
        assert!(bodies.len() >= 2, "expected at least two LLM requests");
        assert!(
            !bodies[0].to_string().contains("Body for alpha."),
            "skill body present before it was loaded"
        );
        assert!(
            bodies[1].to_string().contains("Body for alpha."),
            "loaded skill body missing from the next request"
        );
    }

    /// Skills already loaded before build are projected by the ActiveSkills
    /// adapter on the first request.
    #[test]
    fn preloaded_skill_is_injected_on_first_request() {
        use crate::memory::skill_test_support::skill_registry;
        use claw_skill::{SkillId, SkillSet};

        let http = CapturingHttp::new(vec![body_plain_text("done")]);
        let (builder, _view) = builder_with_view(build_llm(http.clone()), AgentId(1));
        let registry = skill_registry(&[("alpha", "Alpha skill")]);
        let mut skills = SkillSet::new(registry);
        skills
            .load("manifest", SkillId::new("alpha"))
            .expect("load skill");
        let mut agent = builder.with_skills(skills).build().expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "done");

        let body = http.captured_bodies().pop().expect("one captured request");
        assert!(
            body.to_string().contains("Body for alpha."),
            "preloaded skill body missing from first request"
        );
    }

    /// `0` retries fails on the very first disallowed call.
    #[test]
    fn zero_retries_fails_on_first_block() {
        let (builder, _view) = builder_with_view(
            scripted_llm(vec![body_tool_call("t1", "writer", "{}")]),
            AgentId(1),
        );
        // The block budget is the agent's BlockPolicy, configured on the builder.
        let tools = caller_tools().with_active_tools(AllowedTools::new(["end_conversation"]));
        let mut agent = builder
            .with_tools(tools)
            .with_block_retries(0)
            .build()
            .expect("build");

        agent.run("go");
        match agent.tick() {
            TickOutcome::Failed(AgentRunError::ToolNotPermitted { name }) => {
                assert_eq!(name, "writer")
            }
            other => panic!("expected ToolNotPermitted, got {other:?}"),
        }
    }

    /// With no allow-set, gating is off: a tool that gating *would* block runs
    /// normally (the pre-gating behaviour is preserved).
    #[test]
    fn ungated_when_no_allow_set() {
        let (builder, view) = builder_with_view(
            scripted_llm(vec![
                body_tool_call("t1", "writer", "{}"),
                body_end_conversation("done"),
            ]),
            AgentId(1),
        );
        // Note: set_active_tools is intentionally NOT called.
        let mut agent = builder.with_tools(caller_tools()).build().expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "done");
        // The writer tool actually executed.
        assert!(transcript_contents(&view).iter().any(|c| c == "wrote"));
    }

    /// Tool usage prose is projected through the ToolPolicy adapter into the
    /// system prompt.
    #[test]
    fn tool_policy_adapter_injects_tool_usage() {
        let http = CapturingHttp::new(vec![body_plain_text("ok")]);
        let llm = build_llm(Arc::clone(&http));
        let (builder, _view) = builder_with_view(llm, AgentId(1));
        let tools = ToolSet::new([Tool::new(UsageTool)]).expect("tool set");
        let mut agent = builder.with_tools(tools).build().expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "ok");

        let body = http.captured_bodies().pop().expect("one captured request");
        let messages = body["messages"].as_array().expect("messages array");
        let system = messages
            .iter()
            .find(|message| message.get("role").and_then(Value::as_str) == Some("system"))
            .and_then(|message| message.get("content"))
            .and_then(Value::as_str)
            .expect("system message");
        assert!(system.contains("# Tool usage"));
        assert!(system.contains("## usage_tool"));
        assert!(system.contains("checking tool policy projection"));
    }

    /// Clearing the gating restores the defaults: the previously blocked tool runs
    /// again and no phase note is appended to the request.
    #[test]
    fn clearing_gating_restores_ungated_and_no_note() {
        let http = CapturingHttp::new(vec![
            body_tool_call("t1", "writer", "{}"),
            body_end_conversation("done"),
        ]);
        let llm = build_llm(Arc::clone(&http));
        let (builder, view) = builder_with_view(llm, AgentId(1));

        // Gate the set (which also arms the phase note), then immediately ungate
        // before building — clearing must drop both the allow-set and the note,
        // so the agent assembles no phase note.
        let mut tools = caller_tools();
        tools.set_active_tools(AllowedTools::new(["end_conversation"]));
        tools.clear_active_tools();
        let mut agent = builder.with_tools(tools).build().expect("build");

        agent.run("go");
        assert_eq!(run_to_completion(&mut agent), "done");

        // The writer tool executed (gating was cleared).
        assert!(transcript_contents(&view).iter().any(|c| c == "wrote"));

        // No request carried a phase note (the "[system] Tools available" reminder).
        for body in http.captured_bodies() {
            if let Some(messages) = body["messages"].as_array() {
                assert!(
                    messages.iter().all(|m| {
                        m.get("content")
                            .and_then(Value::as_str)
                            .is_none_or(|c| !c.contains("Tools available in the current phase"))
                    }),
                    "a phase note reached the model after gating was cleared"
                );
            }
        }
    }

    /// A registered context adapter pulls the transcript on refresh and lends a
    /// durable context block that reaches the next LLM request.
    #[test]
    fn registered_adapter_pulls_transcript_and_injects_context() {
        use std::sync::Mutex;

        use crate::memory::{ContextAdapter, ContextAdapterInput};
        use claw_context::{Block, BlockKind};

        /// On each refresh, records the transcript version it saw and whether the
        /// user message was present; then lends one global-memory block.
        struct RecordingAdapter {
            seen_versions: Arc<Mutex<Vec<u64>>>,
            saw_user: Arc<Mutex<bool>>,
        }

        impl ContextAdapter for RecordingAdapter {
            fn id(&self) -> &str {
                "recording"
            }
            fn contribute(
                &mut self,
                input: ContextAdapterInput<'_>,
                output: &mut claw_context::ContextSink<'_>,
            ) {
                self.seen_versions
                    .lock()
                    .unwrap_or_else(|p| p.into_inner())
                    .push(input.history.version());
                let saw_user = input.history.messages().as_array().is_some_and(|messages| {
                    messages.iter().any(|message| {
                        message.get("content").and_then(Value::as_str) == Some("hello")
                    })
                });
                if saw_user {
                    *self.saw_user.lock().unwrap_or_else(|p| p.into_inner()) = true;
                }
                output.block(Block::new(
                    BlockKind::GlobalMemory,
                    "Remembered: user likes tea.",
                ));
            }
        }

        let http = CapturingHttp::new(vec![body_plain_text("ok")]);
        let (builder, _view) = builder_with_view(build_llm(http.clone()), AgentId(1));
        let mut agent = builder.build().expect("build");
        let seen_versions = Arc::new(Mutex::new(Vec::new()));
        let saw_user = Arc::new(Mutex::new(false));
        agent
            .register_context_adapter(Box::new(RecordingAdapter {
                seen_versions: Arc::clone(&seen_versions),
                saw_user: Arc::clone(&saw_user),
            }))
            .expect("register adapter");

        agent.run("hello");
        assert_eq!(run_to_completion(&mut agent), "ok");

        // The adapter refreshed at least once and read the user message from the
        // lent transcript (pull, not push).
        assert!(
            !seen_versions.lock().unwrap().is_empty(),
            "adapter never refreshed"
        );
        assert!(
            *saw_user.lock().unwrap(),
            "adapter did not see the user message"
        );

        // The injected block reached the request (carried in the system message).
        let body = http.captured_bodies().pop().expect("one captured request");
        let messages = body["messages"].as_array().expect("messages array");
        let injected = messages.iter().any(|message| {
            message
                .get("content")
                .and_then(Value::as_str)
                .is_some_and(|content| content.contains("Remembered: user likes tea."))
        });
        assert!(
            injected,
            "memory context block missing from request: {messages:?}"
        );
    }

    /// Gating auto-generates a phase note that is appended to the request the model
    /// sees (last message, naming the allowed tools) but is never written to memory.
    #[test]
    fn gating_phase_note_reaches_model_but_not_memory() {
        let http = CapturingHttp::new(vec![body_plain_text("hi there")]);
        let llm = build_llm(Arc::clone(&http));
        let (builder, view) = builder_with_view(llm, AgentId(1));
        // Gating the set arms the note; the agent only places it each tick.
        let tools =
            caller_tools().with_active_tools(AllowedTools::new(["echo", "end_conversation"]));
        let mut agent = builder.with_tools(tools).build().expect("build");

        agent.run("hello");
        assert_eq!(run_to_completion(&mut agent), "hi there");

        // The request carried the auto note as the final (user) message, naming the
        // permitted tools in stable order.
        let body = http.captured_bodies().pop().expect("one captured request");
        let messages = body["messages"].as_array().expect("messages array");
        let last = messages.last().expect("at least one message");
        assert_eq!(last["role"], "user");
        let note = last["content"].as_str().expect("note content");
        assert!(note.contains("Tools available in the current phase"));
        assert!(note.contains("echo"));
        assert!(note.contains("end_conversation"));

        // Memory holds the real turn but not the transient note.
        let committed = transcript_contents(&view);
        assert!(committed.iter().any(|c| c == "hello"));
        assert!(committed.iter().any(|c| c == "hi there"));
        assert!(
            !committed
                .iter()
                .any(|c| c.contains("Tools available in the current phase")),
            "phase note leaked into memory"
        );
    }
}
