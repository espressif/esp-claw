//! Per-session agent runtime: the **graph + scheduler + lifecycle policy**.
//!
//! Each session owns one [`OrchestratorInstance`]. It holds:
//! - an [`AgentRegistry`] — the session's agent *store* (build / get-handle /
//!   remove), graph-blind and schedule-blind;
//! - `meta` — the agent **graph** (parent edge, depth, kind) keyed by [`AgentId`],
//!   owned here so all relationship algorithms are local and lock-free;
//! - the **scheduler** state (`ready`, `parked_approvals`) and the drive loop;
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
//! first delivered message (that message is its goal); later messages append to
//! it.
//!
//! Borrow safety: a tick may emit [`GraphEffect`]s (spawn a child, resolve an
//! approval) through the agent's [`GraphHost`], but those only push onto the
//! instance's effect queue. The instance ticks one agent (locking just that
//! agent's handle), then — with no agent borrowed — drains and applies the queued
//! effects and routes the outcome. Today the drive loop is sequential; the same
//! shape supports concurrent async ticking later (each future locks only its
//! agent).

use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use claw_context::Block;

use crate::agent::base_agent::{
    AgentCommand, AgentCommandError, AgentId, ApprovalDecision, ApprovalId, TickOutcome,
};
use crate::agent::factory::AgentFactory;
use crate::agent::registry::{AgentIdAllocator, AgentRegistry};
use crate::agent::{
    AgentKind, AgentSnapshot, AgentStatus, ApprovalVerdict, GraphEffect, GraphHost,
    TerminationPolicy,
};
use crate::session::SessionId;

/// The kind instantiated as a session's user-facing root agent.
const ROOT_AGENT_KIND: &str = "conversation";

/// A user-facing reply produced by a **root** agent, surfaced to the orchestrator
/// to route to the session's egress.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct RootReply {
    /// The session whose root produced this reply.
    pub(crate) session: SessionId,
    /// The reply text.
    pub(crate) text: String,
    /// True when the root *ended* the conversation (via `end_conversation`),
    /// false for an ordinary yielded answer.
    pub(crate) ended: bool,
}

/// A pending human decision surfaced out of the graph.
///
/// A **subagent**'s pending approval (a permission `Ask`) never reaches here — it
/// bubbles to the session root (the only agent that talks to the user), which
/// classifies the
/// reply and resolves it with `respond_to_approval`. Only a **root**'s own
/// approval is surfaced as an `ApprovalRequest`, to be resolved via
/// [`OrchestratorInstance::resolve_approval`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ApprovalRequest {
    /// The session the requesting agent belongs to.
    pub(crate) session: SessionId,
    /// The agent awaiting the decision (the resolution target).
    pub(crate) agent: AgentId,
    /// The pending approval id to pass back when resolving.
    pub(crate) approval: ApprovalId,
    /// Human-readable description of what needs approving.
    pub(crate) summary: String,
}

/// Everything one [`drive`](OrchestratorInstance::drive) surfaced to the
/// orchestrator: user-facing replies and pending approvals.
///
/// An `Idle`/`Working`/parked tick contributes nothing, so an empty `DriveOutput`
/// means "nothing to route this drive".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct DriveOutput {
    /// Replies a root produced.
    pub(crate) replies: Vec<RootReply>,
    /// Approvals any agent is waiting on.
    pub(crate) approvals: Vec<ApprovalRequest>,
}

impl DriveOutput {
    /// Fold another step's output into this one.
    fn absorb(&mut self, other: DriveOutput) {
        self.replies.extend(other.replies);
        self.approvals.extend(other.approvals);
    }

    fn replies(replies: Vec<RootReply>) -> Self {
        Self {
            replies,
            approvals: Vec::new(),
        }
    }

    fn approval(approval: ApprovalRequest) -> Self {
        Self {
            replies: Vec::new(),
            approvals: vec![approval],
        }
    }
}

/// Failure routing a human decision back to an agent via
/// [`OrchestratorInstance::resolve_approval`].
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum ResolveApprovalError {
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
pub(crate) struct OrchestratorInstance {
    session: SessionId,
    /// The agent store (insert / get-handle / remove). Graph-blind.
    registry: AgentRegistry,
    /// Builds agents (root and children). Owned here; the registry only stores.
    factory: Arc<dyn AgentFactory>,
    /// Shared, process-wide id allocator for roots and spawned children.
    ids: AgentIdAllocator,
    /// The agent graph: one [`NodeMeta`] per live agent, keyed by id.
    meta: HashMap<AgentId, NodeMeta>,
    /// Agents with work queued, in service order.
    ready: VecDeque<AgentId>,
    /// The pending approval id for each agent currently parked on a decision, so a
    /// root can resolve it by agent id alone (it never sees the approval id).
    parked_approvals: HashMap<AgentId, ApprovalId>,
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
    /// The scope-layered prose injected into every agent in this session: the
    /// Global blocks (from the orchestrator) followed by this session's own
    /// blocks, computed once and shared as an `Arc<[Block]>` so every agent's
    /// prefix is byte-identical. Empty until scope providers populate it.
    inherited_context: Arc<[Block<'static>]>,
    /// Monotonic count of external drive cycles (one per delivered user message).
    /// Stamped on the top-level `turn` observability span so a whole drive — and
    /// every nested agent/iteration/tool span under it — reads as one unit.
    turn: u64,
}

impl OrchestratorInstance {
    /// Create an empty instance for `session`. Agents are built with `factory` and
    /// draw ids from the shared `next_agent_id` allocator so they stay unique
    /// across every session.
    ///
    /// `global_context` is the orchestrator-wide prose injected into every agent.
    /// The instance composes it with its own (currently empty) session-scope
    /// blocks once, into the shared `inherited_context` handed to each agent at
    /// build time.
    pub(crate) fn new(
        session: SessionId,
        factory: Arc<dyn AgentFactory>,
        next_agent_id: AgentIdAllocator,
        global_context: Arc<[Block<'static>]>,
    ) -> Self {
        let effects: EffectQueue = Arc::new(Mutex::new(VecDeque::new()));
        let snapshots: SnapshotView = Arc::new(Mutex::new(HashMap::new()));
        let host: Arc<dyn GraphHost> = Arc::new(InstanceHost {
            ids: next_agent_id.clone(),
            effects: Arc::clone(&effects),
            snapshots: Arc::clone(&snapshots),
        });
        // Session-scope blocks are not yet sourced; the inherited set is just the
        // Global layer. When session blocks exist, prepend Global then append
        // Session into one `Arc<[Block]>` here so the composition happens once.
        let inherited_context = global_context;
        Self {
            session,
            registry: AgentRegistry::new(),
            factory,
            ids: next_agent_id,
            meta: HashMap::new(),
            ready: VecDeque::new(),
            parked_approvals: HashMap::new(),
            root: None,
            effects,
            snapshots,
            host,
            inherited_context,
            turn: 0,
        }
    }

    /// Build an agent of `kind` (tasked with `goal`) via the factory and store it,
    /// handing it this instance's [`GraphHost`]. `is_root` gives a session root the
    /// `respond_to_approval` tool. The caller owns the graph/scheduling
    /// bookkeeping; this only builds and stores.
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
        is_root: bool,
    ) -> Result<(), String> {
        let agent = self.factory.create_agent(
            id,
            kind,
            goal,
            Arc::clone(&self.host),
            is_root,
            Arc::clone(&self.inherited_context),
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

    /// The number of live agents in this session.
    #[cfg(test)]
    pub(crate) fn agent_count(&self) -> usize {
        self.registry.count()
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
            Some(root) => {
                self.deliver_message(root, text);
                Ok(())
            }
            None => {
                let id = self.ids.next();
                let kind = AgentKind::new(ROOT_AGENT_KIND);
                self.build_agent(id, &kind, text.into(), true)?;
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

    /// Drive this session's agents until none is ready, collecting every reply and
    /// pending approval. An agent parked on an approval is *not* ready, so the loop
    /// terminates with that approval surfaced for a human decision.
    pub(crate) fn drive(&mut self) -> DriveOutput {
        let mut output = DriveOutput::default();
        // Each `tick_once` drains and applies the effects its tick emitted (before
        // routing the outcome and re-enqueueing), so the ready queue is the single
        // source of truth for "more work to do".
        while self.has_ready() {
            output.absorb(self.tick_once());
        }
        output
    }

    /// Route a human decision back to the agent waiting on `approval`, then mark it
    /// ready so the next [`drive`](Self::drive) resumes it.
    ///
    /// # Errors
    ///
    /// [`ResolveApprovalError::UnknownAgent`] if no live agent has `agent` (e.g. a
    /// finished subagent), or [`ResolveApprovalError::Command`] if the agent is not
    /// awaiting this approval.
    pub(crate) fn resolve_approval(
        &mut self,
        agent: AgentId,
        approval: ApprovalId,
        decision: ApprovalDecision,
    ) -> Result<(), ResolveApprovalError> {
        let handle = self
            .registry
            .get(agent)
            .ok_or(ResolveApprovalError::UnknownAgent(agent))?;
        handle
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .send_command(AgentCommand::ApprovalResult {
                id: approval,
                decision,
            })
            .map_err(ResolveApprovalError::Command)?;
        self.enqueue(agent);
        Ok(())
    }

    /// Tick one ready agent and apply the consequences: apply any graph effects it
    /// emitted this tick (spawns, approval verdicts), then route its outcome (a
    /// subagent result bubbles to its parent; a root reply or pending approval is
    /// surfaced). Returns what to route this step (empty when nothing was ready).
    fn tick_once(&mut self) -> DriveOutput {
        let Some(id) = self.ready.pop_front() else {
            return DriveOutput::default();
        };
        let (kind, depth) = match self.meta.get(&id) {
            Some(meta) => (meta.kind.clone(), meta.depth),
            // The node left the graph since being queued (e.g. a finished child).
            None => return DriveOutput::default(),
        };
        let Some(handle) = self.registry.get(id) else {
            return DriveOutput::default();
        };

        // Publish a consistent graph view before the agent runs, so its
        // `list_subagents` / `watch_subagent` tools read current state.
        self.refresh_snapshots();

        let outcome = {
            // One span per ticked agent; iteration/tool spans nest beneath it.
            // `session` is inherited from the enclosing session span.
            let _span = tracing::info_span!(
                "agent",
                conversation.agent = %id,
                kind = %kind,
                depth = depth
            )
            .entered();
            let mut agent = handle.lock().unwrap_or_else(|poison| poison.into_inner());
            agent.tick()
        };

        // Borrow-safe: the tool calls only emitted effects onto the queue, never
        // touching an agent. Apply them now that no agent is borrowed.
        self.apply_effects();
        self.route_outcome(id, outcome)
    }

    /// Append a user message to the agent `id` and mark it ready. Returns `false`
    /// if no such agent exists.
    fn deliver_message(&mut self, id: AgentId, text: impl Into<String>) -> bool {
        let Some(handle) = self.registry.get(id) else {
            return false;
        };
        // `AppendMessage` is accepted in every state, so this cannot fail.
        let _ = handle
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .send_command(AgentCommand::AppendMessage(text.into()));
        self.enqueue(id);
        true
    }

    /// Record `agent` as parked on `approval`, then either bubble its request to
    /// the session root (a subagent) or surface it to the orchestrator (a root).
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
        self.parked_approvals.insert(agent, approval);
        tracing::info!(agent = %agent, is_root, session = %self.session, "approval parked");

        if is_root {
            // The root talks to the user directly; surface it for a human reply.
            return DriveOutput::approval(ApprovalRequest {
                session: self.session,
                agent,
                approval,
                summary,
            });
        }

        // A subagent: hand the request to the session root to classify, tagged with
        // the requester's id so the root can address it in `respond_to_approval`.
        match self.root {
            Some(root_id) => {
                self.deliver_message(
                    root_id,
                    format!("[approval request from {agent}] {summary}"),
                );
            }
            None => {
                tracing::warn!(session = %self.session, agent = %agent, "no root for session; cannot route approval")
            }
        }
        DriveOutput::default()
    }

    /// Drain and apply every graph effect agents emitted since the last drain.
    /// `Spawn` builds and enqueues a child; `ResolveApproval` resolves a waiting
    /// agent. Applied at a borrow-safe point (no agent is locked), so mutating the
    /// graph is safe.
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
                GraphEffect::ResolveApproval {
                    target,
                    verdict,
                    note,
                } => self.apply_verdict(target, verdict, note),
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
        match self.build_agent(id, &kind, goal, false) {
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

    /// Apply a verdict the root emitted via `respond_to_approval`: look up the
    /// target's parked approval id and resolve it. `yes` approves; `no` and
    /// `other` reject, carrying the note (the user's words) as the reason.
    fn apply_verdict(&mut self, target: AgentId, verdict: ApprovalVerdict, note: Option<String>) {
        let Some(approval) = self.parked_approvals.remove(&target) else {
            tracing::warn!(
                target_agent = %target,
                "respond_to_approval but no approval is parked"
            );
            return;
        };
        let decision = match verdict {
            ApprovalVerdict::Yes => ApprovalDecision::Approved,
            ApprovalVerdict::No | ApprovalVerdict::Other => {
                ApprovalDecision::Rejected(note.unwrap_or_else(|| "rejected".to_string()))
            }
        };
        if let Err(error) = self.resolve_approval(target, approval, decision) {
            tracing::warn!(target_agent = %target, %error, "failed to resolve approval");
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
            // Parked on a human decision. A subagent can't talk to the user, so its
            // request bubbles to the session root for classification; only a root's
            // own approval is surfaced. Either way the agent stays un-enqueued
            // (parked) until resolved.
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
            TickOutcome::Cancelled { reason } => DriveOutput::replies(self.route_result(
                id,
                format!("[cancelled: {reason:?}]"),
                false,
                true,
            )),
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
        if let Some(parent_handle) = self.registry.get(parent_id) {
            parent_handle
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .deliver_child_result(id, text, ok);
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
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use std::collections::VecDeque;
    use std::sync::Mutex;

    use super::*;
    use crate::agent::base_agent::AgentCommand;
    use crate::agent::factory::AgentFactory;
    use crate::agent::{Agent, AgentContext};

    /// The kind every test child is spawned as.
    const TEST_KIND: &str = "worker";

    /// Shared, inspectable record of `(agent, command)` pairs every fake agent
    /// received via `send_command`.
    type CommandLog = Arc<Mutex<Vec<(AgentId, AgentCommand)>>>;
    /// Shared, inspectable record of `(child, text, ok)` results delivered to
    /// parents.
    type DeliveredLog = Arc<Mutex<Vec<(AgentId, String, bool)>>>;

    /// One scripted action a [`FakeAgent`] performs on a `tick`.
    enum Step {
        /// Return `Working` (more to do).
        Work,
        /// Spawn a child via the context with `goal` and `termination`, then return
        /// `Working`.
        Spawn(String, TerminationPolicy),
        /// Return `AwaitingApproval { id: approval-1, summary }`.
        AwaitApproval(String),
        /// Respond to `target`'s approval via the context, then return `Working`
        /// (models a root resolving a subagent's request).
        Respond(AgentId, ApprovalVerdict, Option<String>),
        /// Delete `target` via the context, then return `Working` (models a parent
        /// reaping a persistent subagent).
        Delete(AgentId),
        /// Return `Yielded { text }`.
        Yield(String),
    }

    /// A test agent that replays scripted [`Step`]s, records every command it
    /// receives, and records delivered child results — all into shared vecs. Graph
    /// actions (spawn / respond) go through its [`AgentContext`].
    struct FakeAgent {
        id: AgentId,
        steps: VecDeque<Step>,
        context: AgentContext,
        delivered: DeliveredLog,
        received: CommandLog,
    }

    impl Agent for FakeAgent {
        fn id(&self) -> AgentId {
            self.id
        }

        fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
            self.received.lock().unwrap().push((self.id, command));
            Ok(())
        }

        fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool) {
            self.delivered.lock().unwrap().push((child, text, ok));
        }

        fn tick(&mut self) -> TickOutcome {
            match self.steps.pop_front() {
                Some(Step::Work) => TickOutcome::Working,
                Some(Step::Spawn(goal, termination)) => {
                    self.context
                        .spawn(AgentKind::new(TEST_KIND), None, goal, termination);
                    TickOutcome::Working
                }
                Some(Step::AwaitApproval(summary)) => TickOutcome::AwaitingApproval {
                    id: ApprovalId(1),
                    summary,
                },
                Some(Step::Respond(target, verdict, note)) => {
                    self.context.respond_to_approval(target, verdict, note);
                    TickOutcome::Working
                }
                Some(Step::Delete(target)) => {
                    self.context.delete_subagent(target);
                    TickOutcome::Working
                }
                Some(Step::Yield(text)) => TickOutcome::Yielded { text },
                None => TickOutcome::Idle,
            }
        }
    }

    /// A factory that scripts agents from their goal, sharing one delivered/received
    /// log across every agent it builds (root and children alike).
    ///
    /// Goal grammar:
    /// - `spawn:<g>`  → spawn one child for `<g>`, then yield `final answer`
    ///   (a root that delegates one subtask).
    /// - `deep:<g>`   → spawn one child for `<g>`, then yield `done:deep:<g>`
    ///   (a subagent that itself spawns).
    /// - `approve:<g>`→ park on approval `<g>`, then yield `done:approve:<g>`.
    /// - `rootask:<g>`→ park on approval `<g>`, then yield `done` (a root asking).
    /// - `bubble`     → spawn `approve:delete prod`, let it park, respond to
    ///   `agent-2` with `bubble_verdict`/`bubble_note`, then yield `root-done`.
    /// - anything else→ yield `done:<goal>`.
    struct FakeFactory {
        delivered: DeliveredLog,
        received: CommandLog,
        bubble_verdict: ApprovalVerdict,
        bubble_note: Option<String>,
    }

    impl FakeFactory {
        fn new() -> Self {
            Self {
                delivered: Arc::new(Mutex::new(Vec::new())),
                received: Arc::new(Mutex::new(Vec::new())),
                bubble_verdict: ApprovalVerdict::Yes,
                bubble_note: None,
            }
        }

        fn script(&self, goal: &str) -> VecDeque<Step> {
            let mut steps = VecDeque::new();
            if let Some(rest) = goal.strip_prefix("spawn:") {
                steps.push_back(Step::Spawn(rest.to_string(), TerminationPolicy::AutoOnIdle));
                steps.push_back(Step::Yield("final answer".into()));
            } else if let Some(rest) = goal.strip_prefix("spawnmanual:") {
                steps.push_back(Step::Spawn(rest.to_string(), TerminationPolicy::Manual));
                steps.push_back(Step::Yield("final answer".into()));
            } else if let Some(rest) = goal.strip_prefix("deep:") {
                steps.push_back(Step::Spawn(rest.to_string(), TerminationPolicy::AutoOnIdle));
                steps.push_back(Step::Yield(format!("done:{goal}")));
            } else if let Some(rest) = goal.strip_prefix("deepmanual:") {
                steps.push_back(Step::Spawn(rest.to_string(), TerminationPolicy::Manual));
                steps.push_back(Step::Yield(format!("done:{goal}")));
            } else if let Some(rest) = goal.strip_prefix("approve:") {
                steps.push_back(Step::AwaitApproval(rest.to_string()));
                steps.push_back(Step::Yield(format!("done:{goal}")));
            } else if let Some(rest) = goal.strip_prefix("rootask:") {
                steps.push_back(Step::AwaitApproval(rest.to_string()));
                steps.push_back(Step::Yield("done".into()));
            } else if goal == "reap" {
                // Spawn a persistent child (agent-2), let it settle idle, then
                // delete it and finish.
                steps.push_back(Step::Spawn("leaf".into(), TerminationPolicy::Manual));
                steps.push_back(Step::Work);
                steps.push_back(Step::Delete(AgentId(2)));
                steps.push_back(Step::Yield("reaped".into()));
            } else if goal == "baddelete" {
                // Spawn a persistent child (agent-2), then try to delete a
                // non-descendant id, which must be ignored.
                steps.push_back(Step::Spawn("leaf".into(), TerminationPolicy::Manual));
                steps.push_back(Step::Work);
                steps.push_back(Step::Delete(AgentId(999)));
                steps.push_back(Step::Yield("done".into()));
            } else if goal == "bubble" {
                steps.push_back(Step::Spawn(
                    "approve:delete prod".into(),
                    TerminationPolicy::AutoOnIdle,
                ));
                steps.push_back(Step::Work);
                steps.push_back(Step::Respond(
                    AgentId(2),
                    self.bubble_verdict,
                    self.bubble_note.clone(),
                ));
                steps.push_back(Step::Yield("root-done".into()));
            } else {
                steps.push_back(Step::Yield(format!("done:{goal}")));
            }
            steps
        }
    }

    impl AgentFactory for FakeFactory {
        fn create_agent(
            &self,
            id: AgentId,
            _kind: &AgentKind,
            goal: String,
            host: Arc<dyn GraphHost>,
            _is_root: bool,
            _inherited_context: Arc<[Block<'static>]>,
        ) -> Result<Box<dyn Agent>, String> {
            Ok(Box::new(FakeAgent {
                id,
                steps: self.script(&goal),
                context: AgentContext::new(id, host),
                delivered: Arc::clone(&self.delivered),
                received: Arc::clone(&self.received),
            }))
        }
    }

    fn instance_with(factory: Arc<FakeFactory>, session: SessionId) -> OrchestratorInstance {
        OrchestratorInstance::new(session, factory, AgentIdAllocator::new(), Arc::from([]))
    }

    #[test]
    fn first_message_builds_root_and_drives_it() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(1));
        assert!(instance.root.is_none());

        instance.deliver("hi").unwrap();
        assert!(instance.root.is_some());

        let output = instance.drive();
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "done:hi");
        assert_eq!(output.replies[0].session, SessionId(1));
        // The root persists for the next message.
        assert_eq!(instance.agent_count(), 1);
    }

    #[test]
    fn root_spawns_child_and_receives_its_result() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(1));
        instance.deliver("spawn:subtask").unwrap();

        let output = instance.drive();

        assert_eq!(
            output.replies,
            vec![RootReply {
                session: SessionId(1),
                text: "final answer".into(),
                ended: false,
            }]
        );
        assert!(output.approvals.is_empty());
        // The child's result bubbled back to the root.
        assert!(factory
            .delivered
            .lock()
            .unwrap()
            .iter()
            .any(|(_, text, ok)| text == "done:subtask" && *ok));
        // The one-shot child was removed; only the root remains.
        assert_eq!(instance.agent_count(), 1);
    }

    #[test]
    fn subagent_can_itself_spawn_a_grandchild() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(2));
        instance.deliver("spawn:deep:leaf").unwrap();

        let output = instance.drive();

        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].session, SessionId(2));
        let delivered = factory.delivered.lock().unwrap();
        assert!(
            delivered.iter().any(|(_, text, _)| text == "done:leaf"),
            "grandchild result should reach its parent (the child)"
        );
        assert!(
            delivered
                .iter()
                .any(|(_, text, _)| text == "done:deep:leaf"),
            "child result should reach the root"
        );
        drop(delivered);
        // All subagents are one-shot and removed; only the root remains.
        assert_eq!(instance.agent_count(), 1);
    }

    #[test]
    fn manual_subagent_persists_after_yielding_its_result() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(6));
        // The root spawns a `manual` child, which yields `done:work` and stays.
        instance.deliver("spawnmanual:work").unwrap();

        let output = instance.drive();

        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "final answer");
        // The child still delivered its result to the root...
        assert!(factory
            .delivered
            .lock()
            .unwrap()
            .iter()
            .any(|(_, text, _)| text == "done:work"));
        // ...but, being `manual`, it was kept alive — root + child remain.
        assert_eq!(instance.agent_count(), 2);
    }

    #[test]
    fn removing_a_parent_cascades_to_its_persistent_child() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(7));
        // root --spawn(auto)--> child --spawn(manual)--> grandchild.
        // The grandchild is `manual`, so it would survive its own yield; but its
        // `auto` parent (child) is removed on yield, which must cascade and take the
        // persistent grandchild with it.
        instance.deliver("spawn:deepmanual:leaf").unwrap();

        let output = instance.drive();

        assert_eq!(output.replies.len(), 1);
        let delivered = factory.delivered.lock().unwrap();
        assert!(
            delivered.iter().any(|(_, text, _)| text == "done:leaf"),
            "grandchild result should reach its parent (the child)"
        );
        assert!(
            delivered
                .iter()
                .any(|(_, text, _)| text == "done:deepmanual:leaf"),
            "child result should reach the root"
        );
        drop(delivered);
        // Cascade removed both the auto child and its manual grandchild.
        assert_eq!(instance.agent_count(), 1);
    }

    #[test]
    fn agent_can_delete_its_manual_subagent() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(8));
        instance.deliver("reap").unwrap();

        let output = instance.drive();

        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "reaped");
        // The persistent child existed, then was deleted — only the root remains.
        assert_eq!(instance.agent_count(), 1);
    }

    #[test]
    fn delete_of_a_non_descendant_is_ignored() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(9));
        instance.deliver("baddelete").unwrap();

        let output = instance.drive();

        assert_eq!(output.replies.len(), 1);
        // The bogus delete touched nothing: root + its manual child both survive.
        assert_eq!(instance.agent_count(), 2);
    }

    #[test]
    fn refresh_snapshots_reflects_the_live_graph() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(10));
        // root (agent-1) spawns a persistent child (agent-2) that stays idle.
        instance.deliver("spawnmanual:work").unwrap();
        instance.drive();

        instance.refresh_snapshots();
        let snapshots = instance.snapshots.lock().unwrap();
        assert_eq!(snapshots.len(), 2);

        let root = snapshots.get(&AgentId(1)).expect("root snapshot");
        assert_eq!(root.parent, None);
        assert_eq!(root.depth, 0);

        let child = snapshots.get(&AgentId(2)).expect("child snapshot");
        assert_eq!(child.parent, Some(AgentId(1)));
        assert_eq!(child.depth, 1);
        assert_eq!(child.termination, TerminationPolicy::Manual);
        assert_eq!(child.status, AgentStatus::Idle);
    }

    #[test]
    fn root_approval_is_surfaced_then_resolved_resumes_the_agent() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(Arc::clone(&factory), SessionId(3));
        instance.deliver("rootask:delete prod?").unwrap();
        let root_id = instance.root.unwrap();

        // First drive parks on the approval: no reply, one surfaced request.
        let output = instance.drive();
        assert!(output.replies.is_empty());
        assert_eq!(
            output.approvals,
            vec![ApprovalRequest {
                session: SessionId(3),
                agent: root_id,
                approval: ApprovalId(1),
                summary: "delete prod?".into(),
            }]
        );

        // Resolving re-enqueues the agent; the next drive produces the reply.
        instance
            .resolve_approval(root_id, ApprovalId(1), ApprovalDecision::Approved)
            .expect("resolve approval");
        let output = instance.drive();
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "done");
    }

    #[test]
    fn resolve_approval_for_unknown_agent_errors() {
        let factory = Arc::new(FakeFactory::new());
        let mut instance = instance_with(factory, SessionId(4));
        let result =
            instance.resolve_approval(AgentId(999), ApprovalId(1), ApprovalDecision::Approved);
        assert!(matches!(result, Err(ResolveApprovalError::UnknownAgent(_))));
    }

    /// Drive a session whose root spawns a subagent that parks on an approval,
    /// then has the root respond with `verdict`. Returns the shared command log and
    /// the drive output.
    fn drive_subagent_approval(
        verdict: ApprovalVerdict,
        note: Option<String>,
    ) -> (CommandLog, DriveOutput) {
        let factory = Arc::new(FakeFactory {
            bubble_verdict: verdict,
            bubble_note: note,
            ..FakeFactory::new()
        });
        let received = Arc::clone(&factory.received);
        let mut instance = instance_with(factory, SessionId(5));
        instance.deliver("bubble").unwrap();
        // The root is agent-1; the child it spawns is agent-2 (deterministic).
        assert_eq!(instance.root, Some(AgentId(1)));
        let output = instance.drive();
        (received, output)
    }

    #[test]
    fn subagent_approval_bubbles_to_root_not_to_the_orchestrator() {
        let (received, output) = drive_subagent_approval(ApprovalVerdict::Yes, None);

        // The subagent's request was *not* surfaced to the orchestrator...
        assert!(
            output.approvals.is_empty(),
            "a subagent approval must bubble to the root, not surface"
        );
        // ...it was delivered to the root as a provenance-tagged message.
        let received = received.lock().unwrap();
        assert!(
            received.iter().any(|(agent, command)| *agent == AgentId(1)
                && *command
                    == AgentCommand::AppendMessage(
                        "[approval request from agent-2] delete prod".into()
                    )),
            "root should receive the bubbled approval request: {received:?}"
        );
        // The root still produced its own reply afterwards.
        assert_eq!(output.replies.len(), 1);
        assert_eq!(output.replies[0].text, "root-done");
    }

    #[test]
    fn root_yes_verdict_approves_the_waiting_subagent() {
        let (received, _output) = drive_subagent_approval(ApprovalVerdict::Yes, None);
        let received = received.lock().unwrap();
        assert!(
            received.iter().any(|(agent, command)| *agent == AgentId(2)
                && *command
                    == AgentCommand::ApprovalResult {
                        id: ApprovalId(1),
                        decision: ApprovalDecision::Approved,
                    }),
            "subagent should be approved: {received:?}"
        );
    }

    #[test]
    fn root_no_and_other_verdicts_reject_with_the_user_note() {
        for verdict in [ApprovalVerdict::No, ApprovalVerdict::Other] {
            let (received, _output) = drive_subagent_approval(verdict, Some("not allowed".into()));
            let received = received.lock().unwrap();
            assert!(
                received.iter().any(|(agent, command)| *agent == AgentId(2)
                    && *command
                        == AgentCommand::ApprovalResult {
                            id: ApprovalId(1),
                            decision: ApprovalDecision::Rejected("not allowed".into()),
                        }),
                "verdict {verdict:?} should reject with the note: {received:?}"
            );
        }
    }
}
