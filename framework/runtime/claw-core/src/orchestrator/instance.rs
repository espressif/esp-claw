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
//! first delivered message (that message is its goal); later append deliveries
//! are accepted only once the root has returned to an idle boundary.
//!
//! Borrow safety: a tick may emit [`GraphEffect`]s through the agent's
//! [`GraphHost`], but those only push onto the instance's effect queue. The
//! instance ticks one agent (locking just that agent's handle), then — with no
//! agent borrowed — drains and applies the queued effects and routes the outcome.
//! Today the drive loop is sequential; the same shape supports concurrent async
//! ticking later (each future locks only its agent).

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{
    Agent, AgentCommand, AgentCommandError, AgentId, AgentIdAllocator, AgentKind, AgentPlacement,
    AgentRegistry, AgentSnapshot, AgentStatus, ApprovalDecision, ApprovalId, CancelReason,
    FsAgentFactory, GraphEffect, GraphHost, TerminationPolicy, TickOutcome,
};
use crate::event::EventSink;
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
    /// True when the root *ended* the conversation (via `end_conversation`),
    /// false for an ordinary yielded answer.
    pub ended: bool,
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

    fn reply(session: SessionId, text: String, ended: bool) -> Self {
        Self {
            replies: vec![RootReply {
                session,
                text,
                ended,
            }],
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

struct TickBatch {
    futures: Vec<Option<AgentTickBoxFuture>>,
    outputs: Vec<Option<TickedAgent>>,
}

impl TickBatch {
    fn new(futures: Vec<AgentTickBoxFuture>) -> Self {
        let len = futures.len();
        Self {
            futures: futures.into_iter().map(Some).collect(),
            outputs: std::iter::repeat_with(|| None).take(len).collect(),
        }
    }
}

impl Future for TickBatch {
    type Output = Vec<TickedAgent>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        let mut pending = false;
        for (future_slot, output_slot) in this.futures.iter_mut().zip(this.outputs.iter_mut()) {
            let Some(future) = future_slot else {
                continue;
            };
            match future.as_mut().poll(context) {
                Poll::Ready(output) => {
                    *output_slot = Some(output);
                    *future_slot = None;
                }
                Poll::Pending => pending = true,
            }
        }
        if pending {
            Poll::Pending
        } else {
            Poll::Ready(this.outputs.drain(..).filter_map(|output| output).collect())
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
    /// Builds the root on the first call (the message becomes its goal); appends to
    /// the existing root afterwards. The agent is left ready; call
    /// [`drive`](Self::drive) to run it.
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

    /// Drive until no agent is ready, observing an out-of-band [`SessionControl`]
    /// between ready batches so a concurrent submission can stop the drive in
    /// flight.
    ///
    /// - A **cancel** request aborts the current LLM round (its partial result is
    ///   discarded by the agent's preemption path) and returns
    ///   [`DriveStop::Cancelled`] once the aborted batch unwinds.
    /// - An **interrupt** request lets the current batch finish and commit, then
    ///   returns [`DriveStop::Interrupted`] before the next batch.
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
        while self.has_ready() {
            // Register before awaiting the batch so a cancel arriving during the
            // batch aborts whatever agents are live right now (subagents spawned
            // last batch included).
            let abort_handles = self.registry.abort_handles();
            control.set_cancel_hook(move || {
                for handle in &abort_handles {
                    handle.abort();
                }
            });
            output.absorb(self.tick_ready_batch(events).await);
            // Only honour a control request when there is still work pending: a
            // drive that has already reached quiescence has nothing to interrupt
            // or cancel, so report it as such (the caller delivers any carried
            // message as a fresh turn instead of a continuation).
            if self.has_ready() {
                // Cancel takes precedence: it is the hard stop.
                if control.take_cancel() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Cancelled);
                }
                if control.take_interrupt() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Interrupted);
                }
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

    /// Hard-cancel the session root's current task with `reason` (discarding the
    /// root's open turn and returning it to idle). A no-op when there is no root
    /// or the root has no active task to cancel.
    ///
    /// Paired with a follow-up [`deliver`](Self::deliver) of the new message by
    /// the caller: the two commands land on the root's inbox in order (cancel then
    /// append), so the next drive discards the old open turn and then starts the
    /// fresh task.
    pub(crate) fn cancel_root(&mut self, reason: CancelReason) {
        let Some(root) = self.root else {
            return;
        };
        let Some(agent) = self.registry.get_mut(root) else {
            return;
        };
        // `Cancel` is rejected when the root is already idle (nothing to cancel);
        // that is a benign no-op here, so the error is intentionally ignored.
        if agent.send_command(AgentCommand::Cancel { reason }).is_ok() {
            self.enqueue(root);
        }
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

    pub(crate) fn cancel_active_approval(&mut self, reason: CancelReason) {
        let Some(pending) = self.pop_active_approval() else {
            return;
        };
        let Some(agent) = self.registry.get_mut(pending.agent) else {
            return;
        };
        if agent.send_command(AgentCommand::Cancel { reason }).is_ok() {
            self.enqueue(pending.agent);
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
            return DriveOutput::reply(self.session, approval_prompt(&pending.summary), false);
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

    /// Tick every currently-ready agent as one async batch, then apply the graph
    /// consequences in queue order.
    async fn tick_ready_batch(&mut self, events: &EventSink) -> DriveOutput {
        let ready = self.drain_ready_batch();
        if ready.is_empty() {
            return DriveOutput::default();
        }
        self.refresh_snapshots();

        // The root ticks with the live submission sink; every subagent gets a
        // disabled one, so only root iteration events reach the stream.
        let futures = ready
            .into_iter()
            .map(|ready| {
                let sink = if ready.is_root {
                    events.clone()
                } else {
                    EventSink::disabled()
                };
                tick_agent(ready, sink)
            })
            .collect();
        let ticked = TickBatch::new(futures).await;
        let mut outcomes = Vec::with_capacity(ticked.len());
        for TickedAgent { id, agent, outcome } in ticked {
            self.registry.insert(id, agent);
            outcomes.push((id, outcome));
        }

        self.apply_effects();
        let mut output = DriveOutput::default();
        for (id, outcome) in outcomes {
            if self.meta.contains_key(&id) {
                output.absorb(self.route_outcome(id, outcome));
            }
        }
        output
    }

    fn drain_ready_batch(&mut self) -> Vec<ReadyAgent> {
        let mut batch = Vec::new();
        while let Some(id) = self.ready.pop_front() {
            let Some(meta) = self.meta.get(&id) else {
                continue;
            };
            let Some(agent) = self.registry.take(id) else {
                continue;
            };
            batch.push(ReadyAgent {
                id,
                kind: meta.kind.clone(),
                depth: meta.depth,
                is_root: self.root == Some(id),
                agent,
            });
        }
        batch
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

    /// Record `agent` as parked on `approval`, then surface a normal root reply
    /// prompt when this request becomes the active approval for the session.
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

        self.take_next_approval_prompt()
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
            // orchestrator's job when it deletes the session.
            return vec![RootReply {
                session: self.session,
                text,
                ended,
            }];
        };

        tracing::info!(child_agent = %id, parent_agent = %parent_id, ok, ?termination, "subagent result -> parent");
        if let Some(parent_agent) = self.registry.get_mut(parent_id) {
            parent_agent.deliver_child_result(id, text, ok);
            self.enqueue(parent_id);
        }

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
        tracing::info!(root_agent = %root, removed = victims.len(), session = %self.session, "subtree deleted");
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
