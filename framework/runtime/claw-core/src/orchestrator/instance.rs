//! Per-session agent runtime: the **graph + scheduler + lifecycle policy**.
//!
//! Each session owns one [`OrchestratorInstance`]. It holds:
//! - an [`AgentRegistry`] — the session's agent *store* (build / get-handle /
//!   remove), graph-blind and schedule-blind;
//! - `meta` — the agent **graph** (parent edge, depth, kind) keyed by [`AgentId`],
//!   owned here so all relationship algorithms are local and lock-free;
//! - the **scheduler** state (`ready`, pending approvals) and the drive loop;
//! - `root` — the session's user-facing agent.
//!
//! Responsibility line: the instance decides *when* agents run, *what* their tick
//! outcomes mean (bubble a subagent result to its parent vs. surface a root
//! reply), and *what happens to their lifetimes*. The registry only stores; the
//! agents only compute.
//!
//! Sessions are isolated — one session's agents never appear in another's store —
//! while a single global id allocator (shared at construction) keeps every
//! [`AgentId`] unique across the whole process. The root is built lazily on the
//! first delivered message (that message is its goal); later messages are
//! accepted only once the root has returned to an idle boundary.
//!
//! Borrow safety: a tick may emit [`GraphEffect`]s through the agent's
//! [`GraphHost`], but those only push onto the instance's effect queue. The
//! instance starts ready agents as owned futures, then — with no agent borrowed —
//! drains and applies queued effects and routes each completed outcome. Pending
//! ticks remain in flight, so a slow subagent does not hide completed root output.

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    Agent, AgentAbortHandle, AgentCommand, AgentCommandError, AgentId, AgentIdAllocator, AgentKind,
    AgentPlacement, AgentRegistry, AgentSnapshot, AgentStatus, ApprovalDecision, ApprovalId,
    CancelReason, FsAgentFactory, GraphEffect, GraphHost, TerminationPolicy, TickOutcome,
};
use crate::event::{AgentEvent, EventSink};
use crate::orchestrator::control::{DriveStop, SessionControl};
use crate::session::SessionId;
use tracing::Instrument;

/// The kind instantiated as a session's user-facing root agent.
const ROOT_AGENT_KIND: &str = "conversation";

/// A user-facing reply produced by a **root** agent, surfaced to the channel
/// router.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RootReply {
    /// The session whose root produced this reply.
    pub session: SessionId,
    /// The reply text.
    pub text: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PendingApproval {
    pub(crate) agent: AgentId,
    pub(crate) approval: ApprovalId,
    pub summary: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ParkedApproval {
    approval: ApprovalId,
    summary: String,
    prompted: bool,
}

/// Everything one [`drive`](OrchestratorInstance::drive) surfaced to the
/// orchestrator: user-facing replies only.
///
/// An `Idle`/`Working`/parked tick contributes nothing, so an empty `DriveOutput`
/// means "nothing to route this drive".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct DriveOutput {
    /// Replies a root produced.
    pub replies: Vec<RootReply>,
}

impl DriveOutput {
    /// Fold another step's output into this one.
    pub(crate) fn absorb(&mut self, other: DriveOutput) {
        self.replies.extend(other.replies);
    }

    fn replies(replies: Vec<RootReply>) -> Self {
        Self { replies }
    }

    fn reply(session: SessionId, text: String) -> Self {
        Self {
            replies: vec![RootReply { session, text }],
        }
    }
}

/// Failure routing a human decision back to the active parked approval.
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum ApprovalResolutionError {
    /// The session has no active approval to resolve.
    #[error("no active approval to resolve")]
    NoActiveApproval,
    /// No live agent with the given id (e.g. a finished subagent was removed).
    #[error("no agent {0} to resolve approval for")]
    UnknownAgent(AgentId),
    /// The agent rejected the decision (e.g. it is not awaiting this approval).
    #[error(transparent)]
    Command(AgentCommandError),
}

/// One agent's graph edges — the relationship data the instance owns and runs all
/// graph algorithms over. The agent itself (behind a registry handle) never sees
/// this.
struct NodeMeta {
    /// The creator; `None` for a root.
    parent: Option<AgentId>,
    /// Distance from the root (root = 0); inherited as `parent.depth + 1`.
    depth: u16,
    /// This node's kind (role/template).
    kind: AgentKind,
    /// The spawner-assigned name, or `None` if unnamed (always `None` for a root).
    /// A supervision label only — the node is identified by its [`AgentId`].
    name: Option<String>,
    /// What becomes of this node once it yields (one-shot vs. persistent). A root
    /// is never auto-removed (its result surfaces to the user), so its policy is
    /// irrelevant.
    termination: TerminationPolicy,
}

/// The deferred queue of `(requester, effect)` pairs agents emit during their
/// ticks, drained and applied by the instance at borrow-safe points.
type EffectQueue = Arc<Mutex<VecDeque<(AgentId, GraphEffect)>>>;

/// A shared read-only projection of the graph, refreshed by the instance at the
/// start of each tick so an agent's `list_subagents` / `watch_subagent` tools see
/// a consistent view.
type SnapshotView = Arc<Mutex<HashMap<AgentId, AgentSnapshot>>>;

type AgentTickBoxFuture = Pin<Box<dyn Future<Output = TickedAgent>>>;

struct ReadyAgent {
    id: AgentId,
    kind: AgentKind,
    depth: u16,
    /// Whether this is the session root. Only the root's iteration events are
    /// forwarded to the submission stream; subagents tick with a disabled sink.
    is_root: bool,
    agent: Box<dyn Agent>,
}

struct TickedAgent {
    id: AgentId,
    agent: Box<dyn Agent>,
    outcome: TickOutcome,
}

/// A completed subagent result waiting to be delivered to its parent.
///
/// The parent may be unavailable when the child finishes because the parent is
/// itself in flight, or because it is parked on approval. The scheduler keeps
/// the result here and flushes it when the parent can accept task input again.
struct SubagentResult {
    parent: AgentId,
    child: AgentId,
    text: String,
    ok: bool,
}

/// Session-local table of agent ticks currently in flight.
///
/// This is not a batch barrier: polling resolves as soon as any task completes,
/// leaving slower tasks in the table.
struct InflightAgentTasks {
    entries: Vec<Option<InflightAgentTask>>,
}

struct InflightAgentTask {
    id: AgentId,
    abort: AgentAbortHandle,
    future: AgentTickBoxFuture,
}

impl InflightAgentTasks {
    fn spawn(&mut self, id: AgentId, abort: AgentAbortHandle, future: AgentTickBoxFuture) {
        self.entries
            .push(Some(InflightAgentTask { id, abort, future }));
    }

    fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    fn abort_handles(&self) -> Vec<AgentAbortHandle> {
        self.entries
            .iter()
            .filter_map(|entry| entry.as_ref().map(|entry| entry.abort.clone()))
            .collect()
    }

    fn retain_live(&mut self, meta: &HashMap<AgentId, NodeMeta>) {
        self.entries.retain(|entry| {
            entry
                .as_ref()
                .is_some_and(|entry| meta.contains_key(&entry.id))
        });
    }

    fn next_completed(&mut self) -> CompletedAgentTicks<'_> {
        CompletedAgentTicks { tasks: self }
    }
}

impl Default for InflightAgentTasks {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
        }
    }
}

struct CompletedAgentTicks<'a> {
    tasks: &'a mut InflightAgentTasks,
}

impl Future for CompletedAgentTicks<'_> {
    type Output = Vec<TickedAgent>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        let mut completed = Vec::new();
        let mut pending = false;
        for entry_slot in &mut this.tasks.entries {
            let Some(entry) = entry_slot else {
                continue;
            };
            match entry.future.as_mut().poll(context) {
                Poll::Ready(output) => {
                    completed.push(output);
                    *entry_slot = None;
                }
                Poll::Pending => pending = true,
            }
        }
        this.tasks.entries.retain(Option::is_some);
        if !completed.is_empty() {
            Poll::Ready(completed)
        } else if pending {
            Poll::Pending
        } else {
            Poll::Ready(Vec::new())
        }
    }
}

fn tick_agent(ready: ReadyAgent, events: EventSink) -> AgentTickBoxFuture {
    Box::pin(async move {
        let ReadyAgent {
            id,
            kind,
            depth,
            is_root: _,
            mut agent,
        } = ready;
        let span = tracing::info_span!(
            "agent",
            conversation.agent = %id,
            kind = %kind,
            depth = depth
        );
        let outcome = agent.tick(events).instrument(span).await;
        TickedAgent { id, agent, outcome }
    })
}

/// The instance's [`GraphHost`]: hands agents process-unique ids, queues the
/// graph effects they emit, and serves the current snapshot. Cheap to clone (a
/// few `Arc`s); it never mutates the graph — it only allocates, enqueues, and
/// reads the shared snapshot, so a tool may call it freely mid-tick.
#[derive(Clone)]
struct InstanceHost {
    ids: AgentIdAllocator,
    effects: EffectQueue,
    snapshots: SnapshotView,
}

impl GraphHost for InstanceHost {
    fn next_id(&self) -> AgentId {
        self.ids.next()
    }

    fn emit(&self, requester: AgentId, effect: GraphEffect) {
        self.effects
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .push_back((requester, effect));
    }

    fn snapshot(&self) -> Vec<AgentSnapshot> {
        self.snapshots
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .values()
            .cloned()
            .collect()
    }
}

/// One session's agent store, graph, scheduler, and root.
pub(crate) struct OrchestratorInstance<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    session: SessionId,
    /// The agent store (insert / get-handle / remove). Graph-blind.
    registry: AgentRegistry,
    /// Builds agents (root and children). Owned here; the registry only stores.
    factory: Arc<FsAgentFactory<F, H, Timer>>,
    /// Shared, process-wide id allocator for roots and spawned children.
    ids: AgentIdAllocator,
    /// The agent graph: one [`NodeMeta`] per live agent, keyed by id.
    meta: HashMap<AgentId, NodeMeta>,
    /// Agents with work queued, in service order.
    ready: VecDeque<AgentId>,
    /// Pending permission requests, keyed by the parked agent.
    parked_approvals: HashMap<AgentId, ParkedApproval>,
    /// FIFO order for user-facing approval prompts. The front is the only reply
    /// the next user message may resolve.
    approval_queue: VecDeque<AgentId>,
    /// Finished subagent results whose parent cannot be borrowed yet because it
    /// is either in flight or parked on approval.
    subagent_result_mailbox: VecDeque<SubagentResult>,
    /// The root agent's id, set when the first message builds it.
    root: Option<AgentId>,
    /// Graph effects emitted by agents during the current/last tick, applied after
    /// it at a borrow-safe point. Shared with every agent's [`GraphHost`].
    effects: EffectQueue,
    /// The read-only graph projection agents' inspection tools read, refreshed at
    /// the start of each tick. Shared with every agent's [`GraphHost`].
    snapshots: SnapshotView,
    /// The [`GraphHost`] handed to every agent this instance builds.
    host: Arc<dyn GraphHost>,
    /// Monotonic count of external drive cycles (one per delivered user message).
    /// Stamped on the top-level `turn` observability span so a whole drive — and
    /// every nested agent/iteration/tool span under it — reads as one unit.
    turn: u64,
}

impl<F, H, Timer> OrchestratorInstance<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Create an empty instance for `session`. Agents are built with `factory` and
    /// draw ids from the shared `next_agent_id` allocator so they stay unique
    /// across every session.
    pub(crate) fn new(
        session: SessionId,
        factory: Arc<FsAgentFactory<F, H, Timer>>,
        next_agent_id: AgentIdAllocator,
    ) -> Self {
        let effects: EffectQueue = Arc::new(Mutex::new(VecDeque::new()));
        let snapshots: SnapshotView = Arc::new(Mutex::new(HashMap::new()));
        let host: Arc<dyn GraphHost> = Arc::new(InstanceHost {
            ids: next_agent_id.clone(),
            effects: Arc::clone(&effects),
            snapshots: Arc::clone(&snapshots),
        });
        Self {
            session,
            registry: AgentRegistry::new(),
            factory,
            ids: next_agent_id,
            meta: HashMap::new(),
            ready: VecDeque::new(),
            parked_approvals: HashMap::new(),
            approval_queue: VecDeque::new(),
            subagent_result_mailbox: VecDeque::new(),
            root: None,
            effects,
            snapshots,
            host,
            turn: 0,
        }
    }

    /// Build an agent of `kind` (tasked with `goal`) via the factory and store it,
    /// handing it this instance's [`GraphHost`]. `placement` selects whether
    /// this is the session root or a subagent. The caller owns the
    /// graph/scheduling bookkeeping; this only builds and stores.
    ///
    /// # Errors
    ///
    /// Propagates the factory's error string when the agent cannot be built
    /// (nothing is stored in that case).
    fn build_agent(
        &mut self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        placement: AgentPlacement,
    ) -> Result<(), String> {
        let agent = self.factory.create_agent(
            id,
            kind,
            goal,
            placement,
            Arc::clone(&self.host),
            Arc::from([]),
        )?;
        self.registry.insert(id, agent);
        Ok(())
    }

    /// Advance to the next turn and return its number. A "turn" is one external
    /// drive cycle (a delivered user message); callers stamp it on the top-level
    /// observability span.
    pub(crate) fn next_turn(&mut self) -> u64 {
        self.turn = self.turn.saturating_add(1);
        self.turn
    }

    /// Deliver a user message to this session's root.
    ///
    /// Builds the root on the first call (the message becomes its goal); delivers
    /// to the existing idle root afterwards. The agent is left ready; call
    /// [`drive_interruptible`](Self::drive_interruptible) to run it.
    ///
    /// # Errors
    ///
    /// Propagates the factory's error string when the root cannot be built.
    pub(crate) fn deliver(&mut self, text: impl Into<String>) -> Result<(), String> {
        match self.root {
            Some(root) => self
                .deliver_message(root, text)
                .map_err(|error| format!("delivering to root {root}: {error}")),
            None => {
                let id = self.ids.next();
                let kind = AgentKind::new(ROOT_AGENT_KIND);
                self.build_agent(id, &kind, text.into(), AgentPlacement::Root(self.session))?;
                self.meta.insert(
                    id,
                    NodeMeta {
                        parent: None,
                        depth: 0,
                        kind,
                        name: None,
                        // A root surfaces its result to the user and persists for
                        // the next message; it is never auto-removed.
                        termination: TerminationPolicy::AutoOnIdle,
                    },
                );
                self.root = Some(id);
                self.enqueue(id);
                Ok(())
            }
        }
    }

    /// Drive until no automatic work remains, observing an out-of-band
    /// [`SessionControl`] between completed ticks so a concurrent submission can
    /// stop the drive in flight.
    ///
    /// - A **cancel** request aborts in-flight LLM rounds (partial results are
    ///   discarded by the agents' preemption paths) and returns
    ///   [`DriveStop::Cancelled`] once currently in-flight ticks unwind.
    /// - An **interrupt** request lets in-flight ticks finish and commit, then
    ///   returns [`DriveStop::Interrupted`] before starting more work.
    /// - Otherwise the loop runs to quiescence and returns [`DriveStop::Quiescent`].
    ///
    /// The caller decides the next delivery path from the stop reason; this
    /// method only detects and reports it.
    pub(crate) async fn drive_interruptible(
        &mut self,
        control: &SessionControl,
        events: &EventSink,
    ) -> (DriveOutput, DriveStop) {
        let mut output = DriveOutput::default();
        let mut inflight = InflightAgentTasks::default();
        let mut cancel_requested = false;
        let mut interrupt_requested = false;

        loop {
            if !cancel_requested && !interrupt_requested {
                self.start_ready_agent_tasks(&mut inflight, events);
            }

            if inflight.is_empty() {
                if cancel_requested && self.has_ready() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Cancelled);
                }
                if interrupt_requested && self.has_ready() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Interrupted);
                }
                if self.has_ready() {
                    continue;
                }
                break;
            }

            // Register before awaiting in-flight ticks so a cancel arriving
            // during LLM/tool work aborts the agents that have been taken out of
            // the registry as well as agents still stored there.
            let mut abort_handles = self.registry.abort_handles();
            abort_handles.extend(inflight.abort_handles());
            control.set_cancel_hook(move || {
                for handle in &abort_handles {
                    handle.abort();
                }
            });

            let ticked = inflight.next_completed().await;
            self.route_ticked_agents(ticked, &mut inflight, events);

            if control.take_cancel() {
                cancel_requested = true;
            }
            if control.take_interrupt() {
                interrupt_requested = true;
            }
        }
        control.clear_cancel_hook();
        output.absorb(self.take_next_approval_prompt());
        (output, DriveStop::Quiescent)
    }

    /// Gracefully interrupt the session root with a newer user message, keeping
    /// the task alive. The interruption marker and the message are recorded by
    /// the root agent itself so interrupt semantics stay in the agent layer.
    pub(crate) fn interrupt_root(&mut self, message: impl Into<String>) -> Result<(), String> {
        let message = message.into();
        let Some(root) = self.root else {
            return self.deliver(message);
        };
        let Some(agent) = self.registry.get_mut(root) else {
            return self.deliver(message);
        };
        agent
            .send_command(AgentCommand::Interrupt { message })
            .map_err(|error| error.to_string())?;
        self.enqueue(root);
        Ok(())
    }

    pub(crate) fn active_approval(&self) -> Option<PendingApproval> {
        let agent = *self.approval_queue.front()?;
        let pending = self.parked_approvals.get(&agent)?;
        Some(PendingApproval {
            agent,
            approval: pending.approval,
            summary: pending.summary.clone(),
        })
    }

    pub(crate) fn resolve_active_approval(
        &mut self,
        decision: ApprovalDecision,
    ) -> Result<(), ApprovalResolutionError> {
        let pending = self
            .pop_active_approval()
            .ok_or(ApprovalResolutionError::NoActiveApproval)?;
        self.send_approval_decision(pending.agent, pending.approval, decision)
    }

    /// Hard-cancel every live agent that currently has work. Used by
    /// `SubmitStream::cancel` cleanup after in-flight ticks have unwound, so the
    /// submitted turn does not leave dormant root/subagent work behind.
    pub(crate) fn cancel_all(&mut self, reason: CancelReason) {
        let agents: Vec<AgentId> = self.meta.keys().copied().collect();
        for agent_id in agents {
            let Some(agent) = self.registry.get_mut(agent_id) else {
                continue;
            };
            if agent
                .send_command(AgentCommand::Cancel {
                    reason: reason.clone(),
                })
                .is_ok()
            {
                self.enqueue(agent_id);
            }
        }
    }

    pub(crate) fn take_next_approval_prompt(&mut self) -> DriveOutput {
        loop {
            let Some(agent) = self.approval_queue.front().copied() else {
                return DriveOutput::default();
            };
            let Some(pending) = self.parked_approvals.get_mut(&agent) else {
                self.approval_queue.pop_front();
                continue;
            };
            if pending.prompted {
                return DriveOutput::default();
            }
            pending.prompted = true;
            return DriveOutput::reply(self.session, approval_prompt(&pending.summary));
        }
    }

    /// Route a human decision back to the agent waiting on `approval`, then mark it
    /// ready so the next drive resumes it.
    ///
    /// # Errors
    ///
    /// [`ApprovalResolutionError::UnknownAgent`] if no live agent has `agent` (e.g. a
    /// finished subagent), or [`ApprovalResolutionError::Command`] if the agent is not
    /// awaiting this approval.
    fn send_approval_decision(
        &mut self,
        agent: AgentId,
        approval: ApprovalId,
        decision: ApprovalDecision,
    ) -> Result<(), ApprovalResolutionError> {
        self.registry
            .get_mut(agent)
            .ok_or(ApprovalResolutionError::UnknownAgent(agent))?
            .send_command(AgentCommand::ApprovalResult {
                id: approval,
                decision,
            })
            .map_err(ApprovalResolutionError::Command)?;
        self.enqueue(agent);
        Ok(())
    }

    /// Start every currently-ready agent and retain its future in `inflight`.
    fn start_ready_agent_tasks(&mut self, inflight: &mut InflightAgentTasks, events: &EventSink) {
        self.flush_subagent_result_mailbox();
        let ready = self.drain_ready_agents();
        if ready.is_empty() {
            return;
        }
        self.refresh_snapshots();

        // The root ticks with the live submission sink; every subagent gets a
        // disabled one, so only root iteration events reach the stream.
        for ready in ready {
            let id = ready.id;
            let abort = ready.agent.abort_handle();
            let sink = if ready.is_root {
                events.clone()
            } else {
                EventSink::disabled()
            };
            inflight.spawn(id, abort, tick_agent(ready, sink));
        }
    }

    /// Reinsert completed agents, apply effects, then route completed outcomes.
    ///
    /// Root-visible replies are emitted immediately so a fast foreground agent is
    /// not hidden behind slower in-flight subagents. Approval prompts are held
    /// until quiescence by `take_next_approval_prompt`.
    fn route_ticked_agents(
        &mut self,
        ticked: Vec<TickedAgent>,
        inflight: &mut InflightAgentTasks,
        events: &EventSink,
    ) {
        let mut outcomes = Vec::with_capacity(ticked.len());
        for TickedAgent { id, agent, outcome } in ticked {
            if self.meta.contains_key(&id) {
                self.registry.insert(id, agent);
                outcomes.push((id, outcome));
            }
        }

        self.apply_effects();
        for (id, outcome) in outcomes {
            if self.meta.contains_key(&id) {
                emit_drive_output(events, self.route_outcome(id, outcome));
            }
        }
        inflight.retain_live(&self.meta);
        self.flush_subagent_result_mailbox();
    }

    fn drain_ready_agents(&mut self) -> Vec<ReadyAgent> {
        let mut ready_agents = Vec::new();
        while let Some(id) = self.ready.pop_front() {
            let Some(meta) = self.meta.get(&id) else {
                continue;
            };
            let Some(agent) = self.registry.take(id) else {
                continue;
            };
            ready_agents.push(ReadyAgent {
                id,
                kind: meta.kind.clone(),
                depth: meta.depth,
                is_root: self.root == Some(id),
                agent,
            });
        }
        ready_agents
    }

    /// Append a user message to the idle agent `id` and mark it ready.
    fn deliver_message(&mut self, id: AgentId, text: impl Into<String>) -> Result<(), String> {
        let Some(agent) = self.registry.get_mut(id) else {
            return Err(format!("no such agent {id}"));
        };
        agent
            .send_command(AgentCommand::AppendMessage(text.into()))
            .map_err(|error| error.to_string())?;
        self.enqueue(id);
        Ok(())
    }

    /// Record `agent` as parked on `approval`.
    ///
    /// The user-facing prompt is surfaced only once the session has no automatic
    /// work left to drive, so an approval in one branch does not hide unrelated
    /// in-flight progress.
    fn park_approval(
        &mut self,
        agent: AgentId,
        approval: ApprovalId,
        summary: String,
    ) -> DriveOutput {
        let Some(meta) = self.meta.get(&agent) else {
            return DriveOutput::default();
        };
        let is_root = meta.parent.is_none();
        self.parked_approvals.insert(
            agent,
            ParkedApproval {
                approval,
                summary,
                prompted: false,
            },
        );
        if !self.approval_queue.contains(&agent) {
            self.approval_queue.push_back(agent);
        }
        tracing::info!(agent = %agent, is_root, session = %self.session, "approval parked");

        DriveOutput::default()
    }

    /// Drain and apply every graph effect agents emitted since the last drain.
    /// Applied at a borrow-safe point (no agent is locked), so mutating the graph
    /// is safe.
    fn apply_effects(&mut self) {
        let effects: Vec<(AgentId, GraphEffect)> = self
            .effects
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .drain(..)
            .collect();
        for (requester, effect) in effects {
            match effect {
                GraphEffect::Spawn {
                    id,
                    kind,
                    name,
                    goal,
                    termination,
                } => self.materialize_spawn(requester, id, kind, name, goal, termination),
                GraphEffect::Delete { target } => self.apply_delete(requester, target),
            }
        }
    }

    /// Honor a `delete` an agent requested: remove `target` and its subtree, but
    /// only when `target` is a descendant of `requester` (an agent reaps its own
    /// subagents, never an ancestor, sibling, or unrelated node).
    fn apply_delete(&mut self, requester: AgentId, target: AgentId) {
        if !self.is_descendant(requester, target) {
            tracing::warn!(
                requester_agent = %requester,
                target_agent = %target,
                "delete of a non-descendant ignored"
            );
            return;
        }
        self.delete_subtree(target);
    }

    /// Whether `node` is a strict descendant of `ancestor` (walking parent edges
    /// in `meta`). `false` when `node == ancestor` or the chain never reaches it.
    fn is_descendant(&self, ancestor: AgentId, node: AgentId) -> bool {
        let mut current = self.meta.get(&node).and_then(|meta| meta.parent);
        while let Some(parent) = current {
            if parent == ancestor {
                return true;
            }
            current = self.meta.get(&parent).and_then(|meta| meta.parent);
        }
        false
    }

    /// Rebuild the shared snapshot from the live graph and scheduler state. Status
    /// is derived from what the instance knows: parked on an approval, queued to
    /// run, or otherwise idle.
    fn refresh_snapshots(&self) {
        let mut snapshots = self
            .snapshots
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        snapshots.clear();
        for (&id, meta) in &self.meta {
            let status = if self.parked_approvals.contains_key(&id) {
                AgentStatus::AwaitingApproval
            } else if self.ready.contains(&id) {
                AgentStatus::Ready
            } else {
                AgentStatus::Idle
            };
            snapshots.insert(
                id,
                AgentSnapshot {
                    id,
                    kind: meta.kind.clone(),
                    name: meta.name.clone(),
                    parent: meta.parent,
                    depth: meta.depth,
                    termination: meta.termination,
                    status,
                },
            );
        }
    }

    /// Build a child requested by `parent`, inheriting `parent.depth + 1`. A
    /// request whose parent has vanished is dropped.
    fn materialize_spawn(
        &mut self,
        parent: AgentId,
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        goal: String,
        termination: TerminationPolicy,
    ) {
        let Some(parent_meta) = self.meta.get(&parent) else {
            tracing::warn!(
                parent_agent = %parent,
                child_agent = %id,
                "spawn parent is gone; dropping child"
            );
            return;
        };
        let depth = parent_meta.depth.saturating_add(1);
        match self.build_agent(id, &kind, goal, AgentPlacement::Sub(id)) {
            Ok(()) => {
                tracing::info!(
                    child_agent = %id,
                    parent_agent = %parent,
                    kind = %kind,
                    depth,
                    termination = ?termination,
                    session = %self.session,
                    "subagent materialized"
                );
                self.meta.insert(
                    id,
                    NodeMeta {
                        parent: Some(parent),
                        depth,
                        kind,
                        name,
                        termination,
                    },
                );
                self.enqueue(id);
            }
            Err(error) => {
                tracing::warn!(
                    child_agent = %id,
                    kind = %kind,
                    %error,
                    "failed to create subagent; dropping spawn"
                );
            }
        }
    }

    /// Map a tick outcome to graph actions.
    fn route_outcome(&mut self, id: AgentId, outcome: TickOutcome) -> DriveOutput {
        match outcome {
            // Still working: requeue for another step.
            TickOutcome::Working => {
                self.enqueue(id);
                DriveOutput::default()
            }
            // Waiting for input: leave it parked (no longer ready).
            TickOutcome::Idle => DriveOutput::default(),
            // Parked on a human decision. The agent stays un-enqueued until the
            // orchestrator resolves the queued approval from a user reply.
            TickOutcome::AwaitingApproval {
                id: approval,
                summary,
            } => self.park_approval(id, approval, summary),
            // A finished answer: bubble to parent or surface to the user.
            TickOutcome::Yielded { text } => {
                DriveOutput::replies(self.route_result(id, text, true, false))
            }
            TickOutcome::Ended { final_message } => {
                DriveOutput::replies(self.route_result(id, final_message, true, true))
            }
            TickOutcome::Cancelled { reason } => self.route_cancelled(id, reason),
            TickOutcome::Failed(error) => DriveOutput::replies(self.route_result(
                id,
                format!("[failed: {error:?}]"),
                false,
                true,
            )),
        }
    }

    /// Deliver a finished agent's `text` to its parent (a subagent) or surface it
    /// as a [`RootReply`] (a root). `ended` marks an `end_conversation` close.
    ///
    /// A subagent always reports its result to its parent. What happens to it next
    /// depends on its [`TerminationPolicy`]: an `AutoOnIdle` subagent is removed
    /// (with its whole subtree, so no descendant is orphaned); a `Manual` subagent
    /// survives an ordinary yield (`ok && !ended`) and goes idle, re-taskable and
    /// deletable, but is still removed on a terminal outcome.
    fn route_result(&mut self, id: AgentId, text: String, ok: bool, ended: bool) -> Vec<RootReply> {
        let Some((parent, termination)) = self
            .meta
            .get(&id)
            .map(|meta| (meta.parent, meta.termination))
        else {
            return Vec::new();
        };
        let Some(parent_id) = parent else {
            // A root stays alive (the session persists); cleanup is the
            // orchestrator's job when it deletes the session. `ended` (an
            // `end_conversation` close) is just a normal turn end for a root — it
            // carries no distinct externally-visible signal.
            return vec![RootReply {
                session: self.session,
                text,
            }];
        };

        tracing::info!(child_agent = %id, parent_agent = %parent_id, ok, ?termination, "subagent result -> parent");
        self.deliver_or_mailbox_subagent_result(parent_id, id, text, ok);

        // Keep a `Manual` subagent alive only on an ordinary yield; otherwise remove
        // it (and its subtree, so a persistent grandchild is never left orphaned).
        let keep_alive = termination == TerminationPolicy::Manual && ok && !ended;
        if keep_alive {
            tracing::debug!(agent = %id, "manual subagent yielded; kept alive (idle)");
        } else {
            self.delete_subtree(id);
        }
        Vec::new()
    }

    /// Route a hard cancellation. Cancellation is intentionally silent: no root
    /// reply and no parent message. A cancelled subagent is removed so the graph
    /// does not retain dead work; a cancelled root stays as the reusable session
    /// root.
    fn route_cancelled(&mut self, id: AgentId, reason: CancelReason) -> DriveOutput {
        let Some(parent) = self.meta.get(&id).and_then(|meta| meta.parent) else {
            tracing::info!(agent = %id, ?reason, "root task cancelled");
            return DriveOutput::default();
        };
        tracing::info!(
            agent = %id,
            parent_agent = %parent,
            ?reason,
            "subagent task cancelled"
        );
        self.delete_subtree(id);
        DriveOutput::default()
    }

    /// Remove `root` and every descendant from the store, the graph, the ready
    /// queue, and any parked approvals. Used both for one-shot cleanup and for an
    /// explicit/cascading delete; a parent's removal never leaves orphans.
    fn delete_subtree(&mut self, root: AgentId) {
        let victims = self.subtree_ids(root);
        for victim in &victims {
            self.registry.remove(*victim);
            self.meta.remove(victim);
            self.parked_approvals.remove(victim);
        }
        self.ready.retain(|queued| !victims.contains(queued));
        self.approval_queue
            .retain(|queued| !victims.contains(queued));
        self.subagent_result_mailbox
            .retain(|result| !victims.contains(&result.parent));
        tracing::info!(root_agent = %root, removed = victims.len(), session = %self.session, "subtree deleted");
    }

    fn deliver_or_mailbox_subagent_result(
        &mut self,
        parent: AgentId,
        child: AgentId,
        text: String,
        ok: bool,
    ) {
        if !self.meta.contains_key(&parent) {
            return;
        }
        if self.parked_approvals.contains_key(&parent) {
            self.subagent_result_mailbox.push_back(SubagentResult {
                parent,
                child,
                text,
                ok,
            });
            return;
        }
        if let Some(parent_agent) = self.registry.get_mut(parent) {
            parent_agent.deliver_child_result(child, text, ok);
            self.enqueue(parent);
        } else {
            self.subagent_result_mailbox.push_back(SubagentResult {
                parent,
                child,
                text,
                ok,
            });
        }
    }

    fn flush_subagent_result_mailbox(&mut self) {
        if self.subagent_result_mailbox.is_empty() {
            return;
        }
        let mut pending = VecDeque::new();
        while let Some(result) = self.subagent_result_mailbox.pop_front() {
            if !self.meta.contains_key(&result.parent) {
                continue;
            }
            if self.parked_approvals.contains_key(&result.parent) {
                pending.push_back(result);
                continue;
            }
            let parent = result.parent;
            if let Some(parent_agent) = self.registry.get_mut(parent) {
                parent_agent.deliver_child_result(result.child, result.text, result.ok);
                self.enqueue(parent);
            } else {
                pending.push_back(result);
            }
        }
        self.subagent_result_mailbox = pending;
    }

    /// Collect `root` and all of its descendants (a breadth-first walk of the
    /// parent edges in `meta`).
    fn subtree_ids(&self, root: AgentId) -> Vec<AgentId> {
        let mut out = vec![root];
        let mut frontier = VecDeque::from([root]);
        while let Some(current) = frontier.pop_front() {
            for (&id, meta) in &self.meta {
                if meta.parent == Some(current) {
                    out.push(id);
                    frontier.push_back(id);
                }
            }
        }
        out
    }

    /// Mark `id` ready, avoiding duplicate queue entries.
    fn enqueue(&mut self, id: AgentId) {
        if !self.ready.contains(&id) {
            self.ready.push_back(id);
        }
    }

    /// True while any agent has work queued.
    fn has_ready(&self) -> bool {
        !self.ready.is_empty()
    }

    fn pop_active_approval(&mut self) -> Option<PendingApproval> {
        while let Some(agent) = self.approval_queue.pop_front() {
            let Some(pending) = self.parked_approvals.remove(&agent) else {
                continue;
            };
            return Some(PendingApproval {
                agent,
                approval: pending.approval,
                summary: pending.summary,
            });
        }
        None
    }
}

fn approval_prompt(summary: &str) -> String {
    format!("Permission approval needed:\n{summary}\n\nReply with approval or rejection.")
}

fn emit_drive_output(events: &EventSink, output: DriveOutput) {
    for reply in output.replies {
        events.emit(AgentEvent::Output { text: reply.text });
    }
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;
    use crate::agent::{AgentCommand, AgentCommandError, AgentTickFuture};
    use claw_api::{BackendKind, ClawApiConfig};
    use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp};
    use claw_tool::ToolRegistry;
    use std::sync::{Arc, Mutex};
    use std::task::{Wake, Waker};

    type TestFactory = FsAgentFactory<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;
    type TestInstance =
        OrchestratorInstance<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;
    type ChildEvents = Arc<Mutex<Vec<(AgentId, String, bool)>>>;

    struct NoopAgent {
        id: AgentId,
    }

    impl Agent for NoopAgent {
        fn id(&self) -> AgentId {
            self.id
        }

        fn send_command(&mut self, _command: AgentCommand) -> Result<(), AgentCommandError> {
            Ok(())
        }

        fn deliver_child_result(&mut self, _child: AgentId, _text: String, _ok: bool) {}

        fn deliver_child_input(&mut self, _child: AgentId, _text: String) {}

        fn abort_handle(&self) -> AgentAbortHandle {
            AgentAbortHandle::default()
        }

        fn tick(&mut self, _events: EventSink) -> AgentTickFuture<'_> {
            Box::pin(async { TickOutcome::Idle })
        }
    }

    struct RecordingAgent {
        id: AgentId,
        child_events: ChildEvents,
    }

    impl Agent for RecordingAgent {
        fn id(&self) -> AgentId {
            self.id
        }

        fn send_command(&mut self, _command: AgentCommand) -> Result<(), AgentCommandError> {
            Ok(())
        }

        fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool) {
            self.child_events
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .push((child, text, ok));
        }

        fn deliver_child_input(&mut self, _child: AgentId, _text: String) {}

        fn abort_handle(&self) -> AgentAbortHandle {
            AgentAbortHandle::default()
        }

        fn tick(&mut self, _events: EventSink) -> AgentTickFuture<'_> {
            Box::pin(async { TickOutcome::Idle })
        }
    }

    struct PendingOnce {
        output: Option<TickedAgent>,
        pending: bool,
    }

    impl PendingOnce {
        fn new(output: TickedAgent) -> Self {
            Self {
                output: Some(output),
                pending: true,
            }
        }
    }

    impl Future for PendingOnce {
        type Output = TickedAgent;

        fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
            let this = self.get_mut();
            if this.pending {
                this.pending = false;
                context.waker().wake_by_ref();
                return Poll::Pending;
            }
            Poll::Ready(this.output.take().expect("pending future output present"))
        }
    }

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn ticked_agent(id: AgentId) -> TickedAgent {
        TickedAgent {
            id,
            agent: Box::new(NoopAgent { id }),
            outcome: TickOutcome::Idle,
        }
    }

    fn test_factory() -> Arc<TestFactory> {
        let llm_config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        );
        Arc::new(
            TestFactory::new(Arc::new(ToolRegistry::new()), llm_config, "/mem", &[])
                .expect("factory builds"),
        )
    }

    fn test_instance() -> TestInstance {
        OrchestratorInstance::new(SessionId(1), test_factory(), AgentIdAllocator::new())
    }

    fn insert_meta(instance: &mut TestInstance, id: AgentId, parent: Option<AgentId>, depth: u16) {
        instance.meta.insert(
            id,
            NodeMeta {
                parent,
                depth,
                kind: AgentKind::new("conversation"),
                name: None,
                termination: TerminationPolicy::AutoOnIdle,
            },
        );
    }

    #[test]
    fn inflight_tasks_return_completed_entries_without_waiting_for_pending_entries() {
        let ready_id = AgentId(1);
        let pending_id = AgentId(2);
        let mut inflight = InflightAgentTasks::default();
        inflight.spawn(
            ready_id,
            AgentAbortHandle::default(),
            Box::pin(async move { ticked_agent(ready_id) }),
        );
        inflight.spawn(
            pending_id,
            AgentAbortHandle::default(),
            Box::pin(PendingOnce::new(ticked_agent(pending_id))),
        );

        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);

        let first = {
            let mut next = inflight.next_completed();
            Pin::new(&mut next).poll(&mut context)
        };
        let first = match first {
            Poll::Ready(outputs) => outputs,
            Poll::Pending => panic!("ready tick should not wait for pending tick"),
        };
        assert_eq!(first.len(), 1);
        assert_eq!(first.into_iter().next().expect("one output").id, ready_id);
        assert!(!inflight.is_empty());

        let second = {
            let mut next = inflight.next_completed();
            Pin::new(&mut next).poll(&mut context)
        };
        let second = match second {
            Poll::Ready(outputs) => outputs,
            Poll::Pending => panic!("pending-once tick should complete on second poll"),
        };
        assert_eq!(second.len(), 1);
        assert_eq!(
            second.into_iter().next().expect("one output").id,
            pending_id
        );
        assert!(inflight.is_empty());
    }

    #[test]
    fn subagent_result_mailbox_wakes_parent_after_in_flight_tick_returns() {
        let parent = AgentId(10);
        let child = AgentId(11);
        let child_events = Arc::new(Mutex::new(Vec::new()));
        let mut instance = test_instance();
        insert_meta(&mut instance, parent, None, 0);
        insert_meta(&mut instance, child, Some(parent), 1);

        instance.deliver_or_mailbox_subagent_result(parent, child, "done".to_string(), true);
        assert_eq!(instance.subagent_result_mailbox.len(), 1);
        assert!(!instance.has_ready());

        instance.registry.insert(
            parent,
            Box::new(RecordingAgent {
                id: parent,
                child_events: Arc::clone(&child_events),
            }),
        );
        instance.flush_subagent_result_mailbox();

        assert!(instance.subagent_result_mailbox.is_empty());
        assert_eq!(instance.ready.pop_front(), Some(parent));
        let delivered = child_events
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        assert_eq!(delivered.as_slice(), &[(child, "done".to_string(), true)]);
    }
}
