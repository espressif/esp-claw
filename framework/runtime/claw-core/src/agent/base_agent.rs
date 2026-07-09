//! Layer 2 base agent: a black box that takes **commands** in and reports an
//! outcome out, driven one iteration per [`tick`](BaseAgent::tick).
//!
//! # Command in, outcome out
//!
//! Everything entering the agent is an [`AgentCommand`], queued through the one
//! inbound method [`send_command`](BaseAgent::send_command) — there is no second
//! entry point and no per-command convenience wrapper. Commands queue on an inbox
//! and are reduced by the single funnel `apply_inbound`. Each
//! [`tick`](BaseAgent::tick) returns one [`TickOutcome`]: the agent never has a
//! side-channel of events, because everything it has to report coincides with the
//! moment a tick hands control back.
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
//! on [`Failed`](TickOutcome::Failed). Out-of-band preemption is only an abort
//! signal for the in-flight iteration; task content and task-level control still
//! enter through [`AgentCommand`]. A terminal outcome is reported once and leaves
//! the agent **idle and reusable** — the next
//! [`AppendMessage`](AgentCommand::AppendMessage) starts a fresh task over the
//! same memory and identity once the driver has observed the agent is idle.

use std::borrow::Cow;
use std::collections::{HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use claw_api::{ClawApiAsync, ClawApiConfig, InitError, RetryPolicy};
use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::TranscriptStore;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use strum::IntoStaticStr;

use super::iteration_loop::{
    ChatMessages, CompletedKind, CompletedOutcome, InterruptionControl, IterationId, IterationLoop,
    IterationLoopError, IterationOutcome, IterationResult, IterationStep, PreemptedOutcome,
    SystemPrompt, ToolRun,
};
use crate::agent::manifest::RetryCount;
use crate::agent::tools::{internal_tools, ControlSignal, ControlSink};
use crate::event::EventSink;
use crate::memory::{
    AssistantCommit, ContextAdapter, ContextAdapterInput, SkillContextAdapter, Transcript,
};
use claw_context::{Block, BlockKind, Context};
use claw_permission::{
    Action, Grant, GrantStore, PermissionDecision, PermissionPolicy, PermissionRequest,
};
use claw_skill::SkillSet;
use claw_tool::{ToolGate, ToolSet, ToolSetError};

crate::define_prefixed_id!(AgentId, "agent-", "agent");
crate::define_prefixed_id!(ApprovalId, "approval-", "approval");

crate::define_id_allocator!(
    /// This agent's [`IterationId`] counter. Single-owner (a `BaseAgent` field,
    /// advanced through `&mut self`), so it needs no lock; it is reset to a fresh
    /// counter at the start of each task.
    IterationIdAllocator(IterationId),
    IterationId(0)
);
crate::define_id_allocator!(
    /// This agent's [`ApprovalId`] counter. Single-owner (a `BaseAgent` field,
    /// advanced through `&mut self`), so it needs no lock.
    ApprovalIdAllocator(ApprovalId),
    ApprovalId(0)
);

// ===========================================================================
// Public command / outcome vocabulary
// ===========================================================================

/// Inbound: a control input handed to the agent. This is the agent's entire
/// external surface — the outside drives the agent only through these.
///
/// Notably there is **no `Preempt` command**: the cooperative abort path is the
/// separate [`AgentAbortHandle`] and carries no message payload. New information
/// arrives as [`AppendMessage`](Self::AppendMessage) only at an idle boundary;
/// hard task termination is [`Cancel`](Self::Cancel).
#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
pub(crate) enum AgentCommand {
    /// Start a fresh task with a user message. This is valid only when the agent
    /// is idle; the orchestrator is responsible for deferring append delivery
    /// until that boundary.
    AppendMessage(String),
    /// Abandon the current task. (Orchestrator-initiated hard stop — distinct
    /// from the agent ending itself via `end_conversation`.) Being disruptive,
    /// it discards the still-open turn instead of writing a marker, so cancelled
    /// partial work leaves no transcript trace.
    Cancel {
        /// Why the task is being abandoned; carried on the cancelled outcome.
        reason: CancelReason,
    },
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
pub(crate) enum AgentState {
    /// No active iteration; waiting for an [`AppendMessage`](AgentCommand::AppendMessage).
    Idle,
    /// A task is actively iterating.
    Running,
    /// Waiting on a permission-policy `Ask`, awaiting an
    /// [`ApprovalResult`](AgentCommand::ApprovalResult).
    AwaitingApproval,
}

/// Internal lifecycle state. Unlike the public [`AgentState`], this carries the
/// pending approval id in the state variant so the agent cannot be
/// `AwaitingApproval` without knowing which request it is waiting for.
#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
enum AgentLifecycle {
    Idle,
    Running,
    AwaitingApproval(ApprovalId),
}

impl AgentLifecycle {
    fn public(self) -> AgentState {
        match self {
            Self::Idle => AgentState::Idle,
            Self::Running => AgentState::Running,
            Self::AwaitingApproval(_) => AgentState::AwaitingApproval,
        }
    }
}

/// Rejection of an [`AgentCommand`] that is invalid for the agent's current
/// [`AgentState`].
///
/// The agent is a state machine; not every command is meaningful in every
/// state (e.g. [`Cancel`](AgentCommand::Cancel) while the agent is already
/// idle). A rejected command is
/// **not** enqueued and the agent is left unchanged, so the caller can react
/// without racing a `tick`. Validation is against the state the agent *will* be
/// in once already-queued commands are applied, so batching commands between
/// ticks is sound.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub(crate) enum AgentCommandError {
    /// [`AppendMessage`](AgentCommand::AppendMessage) is only accepted at an idle
    /// boundary. Active-task input must be deferred by the driver.
    #[error("cannot append: the agent is {state:?}, not idle")]
    CannotAppend {
        /// The state the agent was in when append was rejected.
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
#[derive(Clone, Debug, Deserialize, IntoStaticStr, PartialEq, Eq, Serialize)]
pub(crate) enum CancelReason {
    /// A human asked to stop.
    #[strum(serialize = "user_requested")]
    UserRequested,
    /// The host is shutting the agent down.
    #[strum(serialize = "shutdown")]
    Shutdown,
}

/// A human's answer to an approval request.
#[derive(Clone, Debug, Deserialize, IntoStaticStr, PartialEq, Eq, Serialize)]
pub(crate) enum ApprovalDecision {
    /// The human approved; the agent continues.
    #[strum(serialize = "approved")]
    Approved,
    /// The human rejected, with a reason recorded for the agent to reconsider.
    #[strum(serialize = "rejected")]
    Rejected(String),
}

/// What one [`tick`](BaseAgent::tick) did — the agent's sole output channel.
///
/// `Working`/`Idle` are liveness for the driver loop; the rest are one-shot
/// results reported on the tick that produced them. A single tick yields exactly
/// one of these (tool execution is internal — it shows up only as `Working`).
#[derive(Clone, Debug)]
#[must_use]
pub(crate) enum TickOutcome {
    /// Progress was made; call `tick` again promptly.
    Working,
    /// Nothing to do right now (waiting for input or awaiting approval).
    Idle,
    /// The model returned a user-facing answer and handed control back.
    /// **Non-terminal** — the agent goes idle awaiting the next message.
    Yielded {
        /// The model's user-facing answer.
        text: String,
    },
    /// A tool call's permission policy returned `Ask`; the agent is waiting for a
    /// human decision. Resolve it by sending [`AgentCommand::ApprovalResult`].
    AwaitingApproval {
        /// The id to pass back in [`AgentCommand::ApprovalResult`].
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

/// Cause of a terminal [`TickOutcome::Failed`].
///
/// Wraps the lower-level errors a tick can hit: a failed LLM/tool iteration, or a
/// tool refused past the soft-hide retry budget. Context assembly is driven by
/// adapters; adapter-local failures are logged at the adapter boundary, and
/// [`Context::request`] is infallible, so the tick never fails on context
/// assembly.
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentRunError {
    /// One LLM/tool iteration failed.
    #[error(transparent)]
    Iteration(#[from] IterationLoopError),
    /// The model kept calling a tool that soft-hide gating does not permit this
    /// phase, past the allowed retry budget (the agent's
    /// [`BlockPolicy`](claw_tool::BlockPolicy)).
    #[error("tool not permitted in the current phase: {name}")]
    ToolNotPermitted {
        /// The name of the refused tool.
        name: String,
    },
}

/// Failure assembling a [`BaseAgent`] in [`BaseAgent::build`].
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum BaseAgentBuildError {
    /// The per-agent LLM client could not be initialized from the supplied
    /// [`ClawApiConfig`](BaseAgentConfig::llm_config).
    #[error(transparent)]
    Llm(#[from] InitError),
    /// Merging the built-in tool group onto the caller's tools hit a name clash.
    #[error(transparent)]
    Tools(#[from] ToolSetError),
}

// ===========================================================================
// Internals
// ===========================================================================

/// One item on the agent's inbox: either an external [`AgentCommand`] or an
/// internal [`ControlSignal`] raised by a built-in tool. Both flow through the
/// one reducer, but only `Command` is constructible by outside callers.
#[derive(Clone, Deserialize, Serialize)]
enum Inbound {
    Command(AgentCommand),
    TaskInput(String),
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
pub(crate) struct AgentAbortHandle {
    flag: Arc<AtomicBool>,
}

impl AgentAbortHandle {
    /// Abort the in-flight (or next) iteration at its next checkpoint.
    pub fn abort(&self) {
        self.flag.store(true, Ordering::Release);
    }
}

impl Default for AgentAbortHandle {
    /// A standalone handle backed by a fresh flag, wired to no iteration loop.
    ///
    /// Useful for agent implementations that never run the [`IterationLoop`]
    /// (e.g. test doubles): the handle satisfies [`Agent::abort_handle`](crate::agent::Agent::abort_handle)
    /// without observing any real in-flight round.
    fn default() -> Self {
        Self {
            flag: Arc::new(AtomicBool::new(false)),
        }
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

struct PermissionGate<'a> {
    policy: &'a dyn PermissionPolicy,
    grants: &'a GrantStore,
}

impl ToolGate for PermissionGate<'_> {
    fn decide(&self, action: &Action) -> PermissionDecision {
        match self.grants.lookup(&action.signature()) {
            Some(Grant::Granted) => PermissionDecision::Allow,
            Some(Grant::Denied(reason)) => PermissionDecision::Deny {
                reason: reason.clone(),
            },
            None => self.policy.evaluate(&PermissionRequest::new(action)),
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct BlockPolicy {
    retries: u32,
    blocked_rounds: u32,
}

impl BlockPolicy {
    fn new(retries: u32) -> Self {
        Self {
            retries,
            blocked_rounds: 0,
        }
    }

    fn record_round(&mut self, blocked: &[&str]) -> ToolBlockVerdict {
        if blocked.is_empty() {
            self.blocked_rounds = 0;
            return ToolBlockVerdict::Continue;
        }
        self.blocked_rounds = self.blocked_rounds.saturating_add(1);
        if self.blocked_rounds > self.retries {
            return ToolBlockVerdict::Exhausted {
                name: blocked[0].to_string(),
            };
        }
        ToolBlockVerdict::Continue
    }
}

impl Default for BlockPolicy {
    fn default() -> Self {
        Self::new(0)
    }
}

enum ToolBlockVerdict {
    Continue,
    Exhausted { name: String },
}

#[derive(Deserialize, Serialize)]
struct BaseAgentState {
    block_policy: BlockPolicy,
    permission_grants: GrantStore,
    pending_grant_signatures: Vec<String>,
    iterations: IterationIdAllocator,
    approvals: ApprovalIdAllocator,
    lifecycle: AgentLifecycle,
    projected_lifecycle: AgentLifecycle,
    inbox: VecDeque<Inbound>,
}

impl Default for BaseAgentState {
    fn default() -> Self {
        Self {
            block_policy: BlockPolicy::default(),
            permission_grants: GrantStore::new(),
            pending_grant_signatures: Vec::new(),
            iterations: IterationIdAllocator::new(),
            approvals: ApprovalIdAllocator::new(),
            lifecycle: AgentLifecycle::Idle,
            projected_lifecycle: AgentLifecycle::Idle,
            inbox: VecDeque::new(),
        }
    }
}

impl DurableStateCodec for BaseAgentState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }
}

// ===========================================================================
// BaseAgent
// ===========================================================================

/// A base agent that runs one task at a time as a sequence of iterations.
///
/// Build once via [`BaseAgent::build`] from a [`BaseAgentConfig`]; then drive it
/// with commands and ticks. The agent is long-lived and reused across tasks — its
/// conversation memory and identity persist, so finishing a task leaves it ready
/// for the next.
///
/// # Examples
///
/// ```ignore
/// let mut agent = BaseAgent::build(BaseAgentConfig {
///     llm_config,
///     store: memory,
///     tools,
///     skills: SkillSet::empty(),
///     agent_instruction: Block::new(BlockKind::AgentInstruction, "You are a helpful assistant."),
///     inherited_context: Arc::from([]),
///     retry_policy: RetryPolicy::default(),
///     permission_policy: Arc::new(claw_permission::AllowAll),
///     block_retries: RetryCount::new(0),
/// })?;
///
/// agent.send_command(AgentCommand::AppendMessage("summarize today's news".into()))?;
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
pub(crate) struct BaseAgent<H: ClawHttp, Timer: ClawTimer> {
    llm: ClawApiAsync<H, Timer>,
    /// Retry policy applied to every per-iteration LLM call.
    retry_policy: RetryPolicy,
    interruption: AgentInterruption,
    /// The conversation transcript's sole owner. The agent **writes** it directly
    /// at each boundary (a user message, a committed answer/tool patch, or an
    /// explicit end marker) and **reads** it to assemble each request
    /// ([`run_iteration`](Self::run_iteration)); it also lends the read view
    /// ([`Transcript::as_history`]) to each [`ContextAdapter`] so they can pull
    /// from it.
    /// Held behind the [`Transcript`] trait object — the agent never sees the
    /// concrete conversation-memory type, which is why it is not generic over a
    /// filesystem. A `Box` (not `Arc`): the agent is the sole owner of this
    /// handle and only calls `&self` methods; the underlying store's own
    /// `Arc<StoreInner>` already provides the sharing the read adapters need.
    transcript: Box<dyn Transcript>,
    /// The agent's tools, including soft-hide phase gating. The registry/tools
    /// are runtime handles; only the tool-state projection is exported.
    tools: ToolSet,
    /// Runtime permission policy; durable human decisions live in `state`.
    permission_policy: Arc<dyn PermissionPolicy>,
    /// The agent's context assembly, owned wholesale by `claw-context`: inherited
    /// blocks, the agent instruction, adapter-projected blocks/reminders, the
    /// cached system prefix, and the ephemeral reminder tail. The agent does not
    /// hand-place adapter sources; they contribute into a context sink, and
    /// [`Context::request`] renders lazily. Change detection, wire ordering, and
    /// reminder rendering all live in the context.
    context: Context,
    /// Durable agent state. Runtime dependencies stay on [`BaseAgent`].
    state: DurableState<BaseAgentState>,
    /// The actionable outcome produced during the current tick, if any. Reset at
    /// the start of each tick; a single tick produces at most one.
    outcome: Option<TickOutcome>,
    /// Sink the built-in tools push [`ControlSignal`]s onto; drained each tick.
    control: ControlSink,
    /// Registered context adapters. They are request-time projectors; any
    /// authoritative external store they read from is durable on its own.
    adapters: Vec<Box<dyn ContextAdapter>>,
}

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    /// Assemble a runnable agent from a [`BaseAgentConfig`].
    ///
    /// The transcript store is the only place the filesystem type `F` enters, and
    /// it stays on the *config*: the built [`BaseAgent`] erases it behind the
    /// [`History`] / [`Transcript`] trait objects. The caller decides how the
    /// store is built and keyed (via [`TranscriptStore::new`]) and may keep a
    /// clone to inspect the conversation without going through `BaseAgent`:
    ///
    /// ```ignore
    /// let store = TranscriptStore::<MemFs>::new(agent_id, "/data/conversations");
    /// let view = store.clone();
    /// let agent = BaseAgent::build(BaseAgentConfig { llm_config, store, ..config })?;
    /// // later: let messages = view.messages();
    /// ```
    ///
    /// The LLM client is constructed here from [`BaseAgentConfig::llm_config`]
    /// via [`ClawApiAsync::init_default`]: the agent owns its client and its
    /// transports, so callers pass only configuration — never a pre-built client
    /// or transport. The built-in tool group is merged onto the caller's tools.
    /// A configured [`SkillSet`] is wrapped by the skill context adapter, which
    /// contributes `SkillList` and provides the skill-management tool group.
    ///
    /// # Errors
    ///
    /// - [`BaseAgentBuildError::Llm`] if the LLM client cannot be initialized from
    ///   the supplied config (e.g. a missing API key).
    /// - [`BaseAgentBuildError::Tools`] if a built-in tool name clashes with a
    ///   caller tool.
    pub fn build<F: ClawFs + 'static>(
        config: BaseAgentConfig<F>,
    ) -> Result<BaseAgent<H, Timer>, BaseAgentBuildError>
    where
        H: Default,
        Timer: Default,
    {
        let control: ControlSink = Arc::new(Mutex::new(VecDeque::new()));

        let llm = ClawApiAsync::<H, Timer>::init_default(config.llm_config)?;

        let mut tools = config.tools;
        for tool in internal_tools(Arc::clone(&control)) {
            tools.add_tool(tool)?;
        }

        let permission_policy = config.permission_policy;

        // Declare the construction-time blocks the context owns up front: the
        // inherited (Global/Session) blocks and the agent's own instruction block.
        // Tool policy is projected from the current ToolSet in `run_iteration`.
        let mut context = Context::new();
        for block in config.inherited_context.iter() {
            context.with(block.clone());
        }
        context.with(config.agent_instruction);

        // This is the only place `F` is erased. Conversation-history projection
        // is an agent-layer policy, so BaseAgent only installs the intrinsic
        // adapters that carry their own sources and optional tools.
        let skill_adapter: Box<dyn ContextAdapter> =
            Box::new(SkillContextAdapter::new(config.skills));
        let transcript: Box<dyn Transcript> = Box::new(config.store);
        for tool in skill_adapter.tools() {
            tools.add_tool(tool)?;
        }
        let adapters = vec![skill_adapter];

        Ok(BaseAgent {
            llm,
            retry_policy: config.retry_policy,
            interruption: AgentInterruption {
                flag: Arc::new(AtomicBool::new(false)),
            },
            transcript,
            tools,
            permission_policy,
            context,
            state: DurableState::new(BaseAgentState {
                block_policy: BlockPolicy::new(config.block_retries.get()),
                permission_grants: GrantStore::new(),
                pending_grant_signatures: Vec::new(),
                iterations: IterationIdAllocator::new(),
                approvals: ApprovalIdAllocator::new(),
                lifecycle: AgentLifecycle::Idle,
                projected_lifecycle: AgentLifecycle::Idle,
                inbox: VecDeque::new(),
            }),
            outcome: None,
            control,
            adapters,
        })
    }

    // -- Inbound: the command inbox -----------------------------------------

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
    /// state — e.g. [`Cancel`](AgentCommand::Cancel) when already idle, or an
    /// [`ApprovalResult`](AgentCommand::ApprovalResult) when no approval is
    /// pending.
    ///
    /// # Examples
    ///
    /// ```ignore
    /// # use super::{AgentCommand, AgentCommandError, CancelReason};
    ///
    /// // projected state -> Running, then -> Idle
    /// agent.send_command(AgentCommand::AppendMessage("summarize the news".into()))?;
    /// agent.send_command(AgentCommand::Cancel { reason: CancelReason::UserRequested })?;
    ///
    /// // Validated against the *projected* state (the batch so far), before any
    /// // tick runs: cancelling the now-idle agent is rejected and the agent is
    /// // left unchanged.
    /// assert!(matches!(
    ///     agent.send_command(AgentCommand::Cancel { reason: CancelReason::UserRequested }),
    ///     Err(AgentCommandError::NothingToCancel),
    /// ));
    /// # Ok::<(), AgentCommandError>(())
    /// ```
    pub fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        let next = classify(self.state.get().projected_lifecycle, &command)?;
        let state = self.state.get_mut();
        state.projected_lifecycle = next;
        state.inbox.push_back(Inbound::Command(command));
        Ok(())
    }

    /// Queue internal task input from the agent graph (for example a subagent
    /// result). This is deliberately not an [`AgentCommand`]: external user
    /// append is idle-only, while graph events are part of the active task.
    pub(crate) fn push_task_input(&mut self, text: impl Into<String>) {
        let state = self.state.get_mut();
        if state.projected_lifecycle == AgentLifecycle::Idle {
            state.projected_lifecycle = AgentLifecycle::Running;
        }
        state.inbox.push_back(Inbound::TaskInput(text.into()));
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
    /// Call after [`build`](BaseAgent::build) and before driving the agent.
    /// Adapters contribute in registration order, after the agent's own context
    /// blocks (cross-adapter wire order is fixed by [`BlockKind`], not by this
    /// order).
    ///
    /// # Errors
    ///
    /// - [`BaseAgentBuildError::Tools`] if an adapter tool name clashes with an
    ///   existing tool.
    pub fn register_context_adapter(
        &mut self,
        adapter: Box<dyn ContextAdapter>,
    ) -> Result<(), BaseAgentBuildError> {
        for tool in adapter.tools() {
            self.tools.add_tool(tool)?;
        }
        self.adapters.push(adapter);
        Ok(())
    }

    pub(crate) fn restore_state(
        &mut self,
        state: PartStateSlice<'_>,
    ) -> Result<(), DurablePartError> {
        self.state = DurableState::restore_state(state)?;
        self.outcome = None;
        self.interruption.flag.store(false, Ordering::Release);
        Ok(())
    }

    pub(crate) fn durable_parts(&self) -> Vec<&dyn DurablePart> {
        vec![self, &self.tools]
    }

    pub(crate) fn restore_durable_part(
        &mut self,
        name: &str,
        state: PartStateSlice<'_>,
    ) -> Result<bool, DurablePartError> {
        match name {
            "base-agent" => {
                self.restore_state(state)?;
                Ok(true)
            }
            "tool-set" => {
                self.tools.restore_state(state)?;
                Ok(true)
            }
            _ => Ok(false),
        }
    }

    /// Let adapters refresh any async state before the request context is
    /// rendered.
    async fn prepare_adapter_context(
        adapters: &mut [Box<dyn ContextAdapter>],
        history_view: &dyn crate::memory::History,
    ) {
        for adapter in adapters {
            adapter
                .prepare(ContextAdapterInput {
                    history: history_view,
                })
                .await;
        }
    }

    /// Project every registered adapter's source into this agent's context.
    ///
    /// The returned `Value::Array` is the model's history channel. Blocks and
    /// reminders are applied directly to [`Context`]; history messages are cloned
    /// into the request-local sink and ordered by `claw-context` using
    /// [`BlockKind::sort_key`].
    fn render_adapter_context(
        adapters: &mut [Box<dyn ContextAdapter>],
        history_view: &dyn crate::memory::History,
        context: &mut Context,
    ) -> Value {
        let mut sink = context.sink();
        for adapter in adapters {
            adapter.contribute(
                ContextAdapterInput {
                    history: history_view,
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
    /// # use super::{AgentCommand, TickOutcome};
    ///
    /// agent.send_command(AgentCommand::AppendMessage("summarize today's news".into()))?;
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
    pub async fn tick(&mut self, events: &EventSink) -> TickOutcome {
        self.outcome = None;

        // 1. External commands.
        self.drain_inbox();

        // 2. One iteration, if running.
        if self.state.get().lifecycle == AgentLifecycle::Running {
            let iteration_id = self.state.get_mut().iterations.next();

            // Context assembly is owned by `claw-context`: `run_iteration` calls
            // `Context::request`, which re-renders the prefix only if a block
            // changed since last tick. Nothing to prepare or degrade here.
            let outcome = self.run_iteration(iteration_id, events).await;
            self.reduce_outcome(outcome);
            // 3. Internal-tool signals raised during the iteration, folded
            //    back through the same reducer.
            self.drain_control_signals();
            self.drain_inbox();
        }

        // The inbox is now drained; realign the projection with the committed
        // state so the next batch of commands is validated against the truth.
        let lifecycle = self.state.get().lifecycle;
        if self.state.get().projected_lifecycle != lifecycle {
            self.state.get_mut().projected_lifecycle = lifecycle;
        }

        match self.outcome.take() {
            Some(outcome) => outcome,
            None => match self.state.get().lifecycle {
                AgentLifecycle::Running => TickOutcome::Working,
                _ => TickOutcome::Idle,
            },
        }
    }

    /// Run exactly one [`IterationLoop`] round over current context.
    ///
    /// [`Context`] is the single assembler: [`Context::request`] pairs the cached
    /// system prefix with the message tail's two segments — the persisted history
    /// snapshot (a cheap `Arc` clone) and the ephemeral reminder tail — into one
    /// `RequestContext`, re-rendering the prefix only on a real change. No request
    /// stitching happens anywhere else; reminders are never written to memory, so
    /// the cached system/history prefix is untouched.
    async fn run_iteration(
        &mut self,
        iteration_id: IterationId,
        events: &EventSink,
    ) -> IterationResult {
        let tools = self.tools.begin()?;
        let history_view = self.transcript.as_history();
        Self::prepare_adapter_context(&mut self.adapters, history_view).await;
        self.context
            .with(Block::new(BlockKind::ToolPolicy, tools.tool_context()))
            .with_reminder(BlockKind::ToolReminder, Some(tools.extra_tool_context()));
        // Pull each adapter's blocks into the private context and assemble the
        // model's history channel from the adapter `messages()` contributions.
        let history =
            Self::render_adapter_context(&mut self.adapters, history_view, &mut self.context);
        // Take the disjoint field borrows first, then borrow `context` mutably for
        // the (lazily rebuilt) request. Permission policy is a runtime dependency;
        // only the grants it has recorded are durable state.
        let iteration_loop = IterationLoop {
            llm: &mut self.llm,
            interruption: &self.interruption,
            retry: self.retry_policy,
            events,
        };
        let state = self.state.get();
        let permission_gate = PermissionGate {
            policy: self.permission_policy.as_ref(),
            grants: &state.permission_grants,
        };
        let gate = &permission_gate as &dyn ToolGate;
        let context = self.context.request(&history);
        let step = IterationStep {
            iteration_id,
            system_prompt: SystemPrompt(context.system()),
            messages: ChatMessages(context.history()),
            reminders: context.reminders(),
            tools: &tools,
            gate,
        };
        iteration_loop.run(step).await
    }

    // -- Reducer: the single state-mutation funnel for inbound --------------

    fn drain_inbox(&mut self) {
        loop {
            let inbound = if self.state.get().inbox.is_empty() {
                None
            } else {
                self.state.get_mut().inbox.pop_front()
            };
            let Some(inbound) = inbound else {
                break;
            };
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
        if !signals.is_empty() {
            let state = self.state.get_mut();
            for signal in signals {
                state.inbox.push_back(Inbound::Control(signal));
            }
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
            Inbound::Command(AgentCommand::AppendMessage(text)) | Inbound::TaskInput(text) => {
                self.append_task_input(&text);
            }
            Inbound::Command(AgentCommand::Cancel { reason }) => {
                // Cancel is *disruptive* (unlike append/interrupt/approve, which
                // are normal flow): discard the open turn so partial work leaves
                // no transcript trace.
                self.transcript.discard_open_turn();
                self.state.get_mut().pending_grant_signatures.clear();
                // A cancel abandons any in-flight round. If an out-of-band abort
                // set the interruption flag to stop that round but no checkpoint
                // consumed it (e.g. the tick finished before the abort landed),
                // clear it here so it cannot preempt the next task's first
                // iteration.
                self.interruption.flag.store(false, Ordering::Release);
                self.state.get_mut().lifecycle = AgentLifecycle::Idle;
                self.outcome = Some(TickOutcome::Cancelled { reason });
            }
            Inbound::Command(AgentCommand::ApprovalResult { id, decision }) => {
                // The verdict re-enters the transcript as a synthetic user turn.
                let marker = match &decision {
                    ApprovalDecision::Approved => {
                        format!("[approval {id}] approved by the human.")
                    }
                    ApprovalDecision::Rejected(reason) => {
                        format!("[approval {id}] rejected by the human: {reason}")
                    }
                };
                self.transcript.append_user(&marker, false);
                // Record the decision against the asked-about actions so the
                // retried tool calls resolve without asking again.
                self.record_grants(&decision);
                self.state.get_mut().lifecycle = AgentLifecycle::Running;
            }
            Inbound::Control(ControlSignal::EndConversation { final_message }) => {
                self.transcript.commit_ended(&final_message);
                self.state.get_mut().lifecycle = AgentLifecycle::Idle;
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
                    let commit = match answer.raw_message_json.as_deref() {
                        Some(raw) => AssistantCommit::RawJson(raw),
                        None => AssistantCommit::PlainText(&answer.text),
                    };
                    self.transcript.commit_assistant(commit);
                    // Non-terminal: hand back to the caller, go idle for next input.
                    self.state.get_mut().lifecycle = AgentLifecycle::Idle;
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
                    // A permission `Ask` makes the agent wait for a human decision
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
            .filter(|run| run.is_blocked())
            .map(|run| run.name.as_str())
            .collect();
        if !blocked.is_empty() {
            tracing::warn!(name: "tool_gate_blocked", count = blocked.len() as u64);
        }
        if let ToolBlockVerdict::Exhausted { name } =
            self.state.get_mut().block_policy.record_round(&blocked)
        {
            self.fail_with(AgentRunError::ToolNotPermitted { name });
        }
    }

    /// Wait for a human decision when the permission policy asked about any call
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
                run.approval()
                    .map(|approval| (approval.summary.clone(), approval.signature.clone()))
            })
            .collect();
        let Some((summary, _)) = pending.first().cloned() else {
            return;
        };
        let id = {
            let state = self.state.get_mut();
            state.pending_grant_signatures = pending.into_iter().map(|(_, sig)| sig).collect();
            let id = state.approvals.next();
            state.lifecycle = AgentLifecycle::AwaitingApproval(id);
            id
        };
        self.outcome = Some(TickOutcome::AwaitingApproval { id, summary });
    }

    /// Record a human decision against the actions that were asked about, so the
    /// retried calls resolve directly. No-op without a permission gate.
    ///
    /// The agent's [`ApprovalDecision`] is mapped to the permission-layer
    /// [`Grant`] the gate stores (`claw-tool`/`claw-permission` know nothing of
    /// the agent's command vocabulary).
    fn record_grants(&mut self, decision: &ApprovalDecision) {
        let state = self.state.get_mut();
        let signatures = std::mem::take(&mut state.pending_grant_signatures);
        let grant = match decision {
            ApprovalDecision::Approved => Grant::Granted,
            ApprovalDecision::Rejected(reason) => Grant::Denied(reason.clone()),
        };
        let grants = &mut state.permission_grants;
        for signature in signatures {
            match &grant {
                Grant::Granted => grants.grant(signature),
                Grant::Denied(reason) => grants.deny(signature, reason.clone()),
            }
        }
    }

    /// End the task with a failure outcome, leaving the agent idle and reusable.
    fn fail_with(&mut self, error: AgentRunError) {
        self.state.get_mut().lifecycle = AgentLifecycle::Idle;
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
        if outcome.produced.is_empty() {
            return;
        }
        if has_dangling_tool_calls(&outcome.produced.0) {
            let tool_call_count = outcome
                .produced
                .0
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|message| message.get("tool_calls").and_then(Value::as_array))
                .map(|calls| calls.len())
                .sum::<usize>();
            tracing::warn!(
                name: "preempt_patch_dropped",
                tool_call_count = tool_call_count as u64,
            );
            return;
        }
        self.transcript.commit_patch(&outcome.produced.0);
    }

    /// Append one task input turn, starting a new task only when the agent was
    /// idle. Used by both idle-only external append and internal graph events.
    fn append_task_input(&mut self, text: &str) {
        let starts_task = self.state.get().lifecycle == AgentLifecycle::Idle;
        if starts_task {
            let state = self.state.get_mut();
            state.iterations = IterationIdAllocator::new();
            state.lifecycle = AgentLifecycle::Running;
            self.outcome = None;
        }
        // Write the user turn directly; `starts_task` lets the transcript flush
        // any turn a prior task left open before opening a new one.
        self.transcript.append_user(text, starts_task);
    }
}

impl<H: ClawHttp, Timer: ClawTimer> DurablePart for BaseAgent<H, Timer> {
    fn name(&self) -> &'static str {
        "base-agent"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Large,
            change: ChangePatternHint::Arbitrary,
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
// Config
// ===========================================================================

/// All construction-time configuration for a [`BaseAgent`], consumed by
/// [`BaseAgent::build`].
///
/// A plain data struct with public fields and no constructor: fill every field
/// in a struct literal, then hand it to [`BaseAgent::build`]. There is no `new`
/// or builder — the struct literal is the single construction API. This separates
/// the *configure* phase from the *run* phase — the built agent exposes only the
/// command/tick API.
///
/// ```ignore
/// let config = BaseAgentConfig {
///     llm_config,
///     store,
///     tools: my_tools,
///     skills: SkillSet::empty(),
///     agent_instruction: Block::new(BlockKind::AgentInstruction, "You are helpful."),
///     inherited_context: Arc::from([]),
///     retry_policy: RetryPolicy::default(),
///     permission_policy: Arc::new(claw_permission::AllowAll),
///     block_retries: RetryCount::new(0),
/// };
/// let agent = BaseAgent::build(config)?;
/// ```
pub(crate) struct BaseAgentConfig<F: ClawFs + 'static> {
    /// Config for the per-agent LLM client. [`BaseAgent::build`] builds the client
    /// internally via [`ClawApiAsync::init_default`], so the caller supplies
    /// configuration, not a pre-constructed client: building the client outside
    /// only to hand it in adds no value.
    pub llm_config: ClawApiConfig,
    /// The caller-owned transcript store — the only place the filesystem type `F`
    /// enters; the built agent erases it behind trait objects.
    pub store: TranscriptStore<F>,
    /// The agent's tools. The built-in control tool is added during
    /// [`BaseAgent::build`].
    pub tools: ToolSet,
    /// The agent's skills; [`SkillSet::empty`] for no skills.
    pub skills: SkillSet,
    /// The agent's own instruction block (its persona/instructions). An empty
    /// block renders no system message. Defaults to an empty
    /// [`BlockKind::AgentInstruction`] block.
    pub agent_instruction: Block<'static>,
    /// Scope-layered prose blocks injected from above (Global, then Session) that
    /// render ahead of the agent's own instruction. Shared as an `Arc<[Block]>` so
    /// several agents reference one computed set for byte-identical prefixes.
    pub inherited_context: Arc<[Block<'static>]>,
    /// The [`RetryPolicy`] applied to every per-iteration LLM call.
    pub retry_policy: RetryPolicy,
    /// A permission policy that gates every tool call (`Allow` / `Ask` / `Deny`).
    /// Use [`claw_permission::AllowAll`] when every call that passes soft-hide
    /// gating should run.
    pub permission_policy: Arc<dyn PermissionPolicy>,
    /// How many consecutive soft-hide-blocked tool rounds to tolerate before the
    /// task fails with [`ToolNotPermitted`](AgentRunError::ToolNotPermitted).
    /// [`RetryCount::new(0)`](RetryCount) fails on the first blocked round.
    pub block_retries: RetryCount,
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
/// The match is exhaustive over every `(state, command)` pair so a new state or
/// command cannot be silently mishandled.
fn classify(
    state: AgentLifecycle,
    command: &AgentCommand,
) -> Result<AgentLifecycle, AgentCommandError> {
    use AgentCommand as Command;
    use AgentLifecycle as State;
    match (state, command) {
        // External append starts a fresh task only at an idle boundary. Active
        // task input is queued by the orchestrator until that boundary.
        (State::Idle, Command::AppendMessage(_)) => Ok(State::Running),
        (state @ (State::Running | State::AwaitingApproval(_)), Command::AppendMessage(_)) => {
            Err(AgentCommandError::CannotAppend {
                state: state.public(),
            })
        }
        // Cancel ends an active task; there is nothing to cancel when idle.
        (State::Idle, Command::Cancel { .. }) => Err(AgentCommandError::NothingToCancel),
        (State::Running | State::AwaitingApproval(_), Command::Cancel { .. }) => Ok(State::Idle),

        // An approval result needs a matching pending request.
        (State::AwaitingApproval(pending), Command::ApprovalResult { id, .. }) => {
            if pending == *id {
                Ok(State::Running)
            } else {
                Err(AgentCommandError::ApprovalMismatch {
                    expected: pending,
                    got: *id,
                })
            }
        }
        (state @ (State::Idle | State::Running), Command::ApprovalResult { .. }) => {
            Err(AgentCommandError::NotAwaitingApproval {
                state: state.public(),
            })
        }
    }
}
