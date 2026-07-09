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
use std::borrow::Cow;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::error::Error;
use std::fmt;
use std::sync::{Arc, Mutex};

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, SchemaVersion, StorageHint, StorageSizeHint,
};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use serde::{Deserialize, Serialize};
use tracing::Instrument as _;

use crate::agent::{
    Agent, AgentAbortHandle, AgentCommand, AgentCommandError, AgentId, AgentIdAllocator, AgentKind,
    AgentPlacement, AgentRegistry, AgentSnapshot, AgentStatus, ApprovalDecision, ApprovalId,
    CancelReason, FsAgentCreateError, FsAgentFactory, GraphEffect, GraphHost, TerminationPolicy,
    TickOutcome,
};
use crate::event::{EventSink, SessionEvent};
use crate::orchestrator::control::{DriveControl, DriveStop};
use crate::orchestrator::InstanceWork;
use crate::session::SessionId;

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

/// Failure accepting user text into this session's root agent.
#[derive(Debug, thiserror::Error)]
pub(crate) enum InstanceDeliverError {
    /// The lazy root agent could not be built for the first message.
    #[error("failed to build root agent: {0}")]
    Create(#[from] FsAgentCreateError),
    /// Existing-root delivery failed.
    #[error("failed to deliver to root {root}: {source}")]
    Root {
        /// The root agent id.
        root: AgentId,
        /// The delivery failure.
        #[source]
        source: AgentMessageDeliveryError,
    },
}

#[derive(Debug)]
pub struct OrchestratorInstanceRestoreError {
    kind: OrchestratorInstanceRestoreErrorKind,
}

#[derive(Debug, thiserror::Error)]
enum OrchestratorInstanceRestoreErrorKind {
    #[error("failed to rebuild checkpointed agent {agent}: {source}")]
    Agent {
        agent: AgentId,
        #[source]
        source: FsAgentCreateError,
    },
    #[error("checkpointed agent is missing after rebuild: {0}")]
    MissingAgent(AgentId),
    #[error("unknown checkpointed agent part {part} for {agent}")]
    UnknownPart { agent: AgentId, part: String },
    #[error("failed to restore checkpointed agent part {part} for {agent}: {source}")]
    DurablePart {
        agent: AgentId,
        part: String,
        #[source]
        source: DurablePartError,
    },
}

impl OrchestratorInstanceRestoreError {
    fn agent(agent: AgentId, source: FsAgentCreateError) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::Agent { agent, source },
        }
    }

    fn missing_agent(agent: AgentId) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::MissingAgent(agent),
        }
    }

    fn unknown_part(agent: AgentId, part: String) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::UnknownPart { agent, part },
        }
    }

    fn durable_part(agent: AgentId, part: String, source: DurablePartError) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::DurablePart {
                agent,
                part,
                source,
            },
        }
    }
}

impl fmt::Display for OrchestratorInstanceRestoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.kind.fmt(formatter)
    }
}

impl Error for OrchestratorInstanceRestoreError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        self.kind.source()
    }
}

/// Failure appending a message to one live agent.
#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentMessageDeliveryError {
    /// No live agent with the given id.
    #[error("no such agent: {0}")]
    UnknownAgent(AgentId),
    /// The agent rejected the append command in its current state.
    #[error(transparent)]
    Command(#[from] AgentCommandError),
}

/// One agent's graph edges — the relationship data the instance owns and runs all
/// graph algorithms over. The agent itself (behind a registry handle) never sees
/// this.
#[derive(Clone)]
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
    /// Whether this is the session root. Only the root's iteration events are
    /// forwarded to the session stream; subagents tick with a disabled sink.
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
#[derive(Default)]
struct InflightAgentTasks {
    entries: Vec<Option<InflightAgentTask>>,
}

struct InflightAgentTask {
    id: AgentId,
    is_root: bool,
    abort: AgentAbortHandle,
    future: AgentTickBoxFuture,
}

impl InflightAgentTasks {
    fn spawn(
        &mut self,
        id: AgentId,
        is_root: bool,
        abort: AgentAbortHandle,
        future: AgentTickBoxFuture,
    ) {
        self.entries.push(Some(InflightAgentTask {
            id,
            is_root,
            abort,
            future,
        }));
    }

    fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    fn has_root(&self) -> bool {
        self.entries.iter().flatten().any(|entry| entry.is_root)
    }

    fn has_background(&self) -> bool {
        self.entries.iter().flatten().any(|entry| !entry.is_root)
    }

    fn abort_handles(&self) -> Vec<AgentAbortHandle> {
        self.entries
            .iter()
            .filter_map(|entry| entry.as_ref().map(|entry| entry.abort.clone()))
            .collect()
    }

    fn retain_live(&mut self, meta: &BTreeMap<AgentId, NodeMeta>) {
        self.entries.retain(|entry| {
            entry
                .as_ref()
                .is_some_and(|entry| meta.contains_key(&entry.id))
        });
    }

    fn next_completed<'a>(&'a mut self, control: &'a DriveControl) -> CompletedAgentTicks<'a> {
        CompletedAgentTicks {
            tasks: self,
            control,
        }
    }
}

struct CompletedAgentTicks<'a> {
    tasks: &'a mut InflightAgentTasks,
    control: &'a DriveControl,
}

impl Future for CompletedAgentTicks<'_> {
    type Output = Vec<TickedAgent>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        this.control.set_waker(context.waker().clone());
        if this.control.has_signal() {
            return Poll::Ready(Vec::new());
        }
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
            is_root,
            mut agent,
            ..
        } = ready;
        let outcome = agent.tick(events).await;
        match &outcome {
            TickOutcome::AwaitingApproval { id, .. } => {
                tracing::info!(name: "awaiting_approval", approval = %id);
            }
            TickOutcome::Cancelled { reason } => {
                let reason: &'static str = reason.into();
                if is_root {
                    tracing::warn!(name: "root_cancelled", reason);
                } else {
                    tracing::warn!(name: "subagent_cancelled", agent = %id, reason);
                }
            }
            TickOutcome::Failed(_) => {
                tracing::error!(name: "task_failed", "");
            }
            _ => {}
        }
        TickedAgent { id, agent, outcome }
    })
}

/// The instance's [`GraphHost`]: hands agents process-unique ids, queues the
/// graph effects they emit, and serves the current snapshot. Cheap to clone (a
/// few `Arc`s); it never mutates the graph — it only allocates, enqueues, and
/// reads the shared snapshot, so a tool may call it freely mid-tick.
#[derive(Clone)]
struct InstanceHost {
    agent_id_allocator: AgentIdAllocator,
    effects: EffectQueue,
    snapshots: SnapshotView,
}

impl GraphHost for InstanceHost {
    fn next_id(&self) -> AgentId {
        self.agent_id_allocator.next()
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
pub(crate) struct OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    session: SessionId,
    /// Builds agents (root and children). Owned here; the registry only stores.
    factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
    /// Shared, process-wide id allocator for roots and spawned children.
    agent_id_allocator: AgentIdAllocator,
    /// Durable graph and scheduler state.
    state: DurableState<OrchestratorInstanceState>,
    /// Agent ticks currently in flight. Kept on the session instance so a root
    /// turn can end while background subagents continue running.
    inflight: InflightAgentTasks,
    /// Graph effects emitted by agents during the current/last tick, applied after
    /// it at a borrow-safe point. Shared with every agent's [`GraphHost`].
    effects: EffectQueue,
    /// The read-only graph projection agents' inspection tools read, refreshed at
    /// the start of each tick. Shared with every agent's [`GraphHost`].
    snapshots: SnapshotView,
    /// The [`GraphHost`] handed to every agent this instance builds.
    host: Arc<dyn GraphHost>,
}

#[derive(Default)]
pub(crate) struct OrchestratorInstanceState {
    /// The agent store (insert / get-handle / remove). Graph-blind.
    registry: AgentRegistry,
    /// The root agent's id, set when the first message builds it.
    root: Option<AgentId>,
    /// The agent graph: one [`NodeMeta`] per live agent, keyed by id.
    meta: BTreeMap<AgentId, NodeMeta>,
    /// Agents with work queued, in service order.
    ready: VecDeque<AgentId>,
    /// Pending permission requests, keyed by the parked agent.
    parked_approvals: BTreeMap<AgentId, ParkedApproval>,
    /// FIFO order for user-facing approval prompts. The front is the only reply
    /// the next user message may resolve.
    approval_queue: VecDeque<AgentId>,
    /// Finished subagent results whose parent cannot be borrowed yet because it
    /// is either in flight or parked on approval.
    subagent_result_mailbox: VecDeque<SubagentResult>,
    /// Agent durable payloads decoded from checkpoint and awaiting runtime
    /// agent reconstruction.
    pending_agent_parts: BTreeMap<AgentId, Vec<AgentPartState>>,
}

#[derive(Deserialize, Serialize)]
struct OrchestratorInstanceSnapshot {
    root: Option<AgentId>,
    agents: Vec<AgentNodeSnapshot>,
    ready_queue: Vec<AgentId>,
    parked_approvals: Vec<ParkedApprovalSnapshot>,
    approval_queue: Vec<AgentId>,
    subagent_result_mailbox: Vec<SubagentResultSnapshot>,
    #[serde(default)]
    agent_parts: Vec<AgentPartsSnapshot>,
}

#[derive(Deserialize, Serialize)]
struct AgentNodeSnapshot {
    id: AgentId,
    parent: Option<AgentId>,
    depth: u16,
    kind: String,
    name: Option<String>,
    termination_policy: String,
}

#[derive(Deserialize, Serialize)]
struct ParkedApprovalSnapshot {
    agent: AgentId,
    approval: ApprovalId,
    summary: String,
    prompted: bool,
}

#[derive(Deserialize, Serialize)]
struct SubagentResultSnapshot {
    parent: AgentId,
    child: AgentId,
    text: String,
    ok: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct AgentPartsSnapshot {
    id: AgentId,
    parts: Vec<AgentPartState>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct AgentPartState {
    name: String,
    schema_version: SchemaVersion,
    bytes: Vec<u8>,
}

impl Serialize for OrchestratorInstanceState {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::Error as _;

        let mut agents = Vec::with_capacity(self.meta.len());
        for (&id, meta) in &self.meta {
            agents.push(AgentNodeSnapshot {
                id,
                parent: meta.parent,
                depth: meta.depth,
                kind: meta.kind.as_str().to_string(),
                name: meta.name.clone(),
                termination_policy: meta.termination.as_str().to_string(),
            });
        }
        let parked_approvals: Vec<ParkedApprovalSnapshot> = self
            .parked_approvals
            .iter()
            .map(|(&agent, pending)| ParkedApprovalSnapshot {
                agent,
                approval: pending.approval,
                summary: pending.summary.clone(),
                prompted: pending.prompted,
            })
            .collect();

        OrchestratorInstanceSnapshot {
            root: self.root,
            agents,
            ready_queue: self.ready.iter().copied().collect(),
            parked_approvals,
            approval_queue: self.approval_queue.iter().copied().collect(),
            subagent_result_mailbox: self
                .subagent_result_mailbox
                .iter()
                .map(|result| SubagentResultSnapshot {
                    parent: result.parent,
                    child: result.child,
                    text: result.text.clone(),
                    ok: result.ok,
                })
                .collect(),
            agent_parts: self
                .registry
                .iter()
                .map(|(id, agent)| {
                    let parts = agent
                        .durable_parts()
                        .into_iter()
                        .map(|part| {
                            let state = part.export_state().map_err(S::Error::custom)?;
                            Ok(AgentPartState {
                                name: part.name().to_owned(),
                                schema_version: state.schema_version,
                                bytes: state.bytes.into_owned(),
                            })
                        })
                        .collect::<Result<Vec<_>, S::Error>>()?;
                    Ok(AgentPartsSnapshot { id, parts })
                })
                .collect::<Result<Vec<_>, S::Error>>()?,
        }
        .serialize(serializer)
    }
}

impl DurableStateCodec for OrchestratorInstanceState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        let snapshot: OrchestratorInstanceSnapshot =
            serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)?;
        let mut meta = BTreeMap::new();
        for agent in snapshot.agents {
            meta.insert(
                agent.id,
                NodeMeta {
                    parent: agent.parent,
                    depth: agent.depth,
                    kind: AgentKind::new(agent.kind),
                    name: agent.name,
                    termination: match agent.termination_policy.as_str() {
                        "auto" => TerminationPolicy::AutoOnIdle,
                        "manual" => TerminationPolicy::Manual,
                        _ => {
                            return Err(DurablePartError::InvalidState(
                                "unknown termination policy",
                            ))
                        }
                    },
                },
            );
        }
        let parked_approvals = snapshot
            .parked_approvals
            .into_iter()
            .map(|approval| {
                (
                    approval.agent,
                    ParkedApproval {
                        approval: approval.approval,
                        summary: approval.summary,
                        prompted: approval.prompted,
                    },
                )
            })
            .collect();
        let subagent_result_mailbox = snapshot
            .subagent_result_mailbox
            .into_iter()
            .map(|result| SubagentResult {
                parent: result.parent,
                child: result.child,
                text: result.text,
                ok: result.ok,
            })
            .collect();
        let pending_agent_parts = snapshot
            .agent_parts
            .into_iter()
            .map(|agent| (agent.id, agent.parts))
            .collect();
        Ok(Self {
            registry: AgentRegistry::new(),
            root: snapshot.root,
            meta,
            ready: snapshot.ready_queue.into(),
            parked_approvals,
            approval_queue: snapshot.approval_queue.into(),
            subagent_result_mailbox,
            pending_agent_parts,
        })
    }
}

impl<Filesystem, Http, Timer> DurablePart for OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn name(&self) -> &'static str {
        "orchestrator-instance"
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

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Create an empty instance for `session`. Agents are built with `factory` and
    /// draw ids from the shared agent id allocator so they stay unique
    /// across every session.
    pub(crate) fn new(
        session: SessionId,
        factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        state: OrchestratorInstanceState,
    ) -> Self {
        let effects: EffectQueue = Arc::new(Mutex::new(VecDeque::new()));
        let snapshots: SnapshotView = Arc::new(Mutex::new(HashMap::new()));
        let host: Arc<dyn GraphHost> = Arc::new(InstanceHost {
            agent_id_allocator: agent_id_allocator.clone(),
            effects: Arc::clone(&effects),
            snapshots: Arc::clone(&snapshots),
        });
        Self {
            session,
            factory,
            agent_id_allocator,
            state: DurableState::new(state),
            inflight: InflightAgentTasks::default(),
            effects,
            snapshots,
            host,
        }
    }

    pub(crate) fn from_restored_state(
        session: SessionId,
        factory: Arc<FsAgentFactory<Filesystem, Http, Timer>>,
        agent_id_allocator: AgentIdAllocator,
        state: OrchestratorInstanceState,
    ) -> Result<Self, OrchestratorInstanceRestoreError> {
        let mut instance = Self::new(session, factory, agent_id_allocator, state);
        instance.restore_agents_from_pending_parts()?;
        Ok(instance)
    }

    fn restore_agents_from_pending_parts(
        &mut self,
    ) -> Result<(), OrchestratorInstanceRestoreError> {
        let pending = std::mem::take(&mut self.state.get_mut().pending_agent_parts);
        let agents = self
            .state
            .get()
            .meta
            .iter()
            .map(|(&id, meta)| (id, meta.clone()))
            .collect::<Vec<_>>();
        for (id, meta) in agents {
            let placement = if self.state.get().root == Some(id) {
                AgentPlacement::Root(self.session)
            } else {
                AgentPlacement::Sub(id)
            };
            self.build_agent(id, &meta.kind, String::new(), placement)
                .map_err(|source| OrchestratorInstanceRestoreError::agent(id, source))?;

            let Some(parts) = pending.get(&id) else {
                continue;
            };
            let state = self.state.get_mut();
            let agent = state
                .registry
                .get_mut(id)
                .ok_or_else(|| OrchestratorInstanceRestoreError::missing_agent(id))?;
            for part in parts {
                let restored = agent
                    .restore_durable_part(
                        &part.name,
                        PartStateSlice {
                            schema_version: part.schema_version,
                            bytes: &part.bytes,
                        },
                    )
                    .map_err(|source| {
                        OrchestratorInstanceRestoreError::durable_part(
                            id,
                            part.name.clone(),
                            source,
                        )
                    })?;
                if !restored {
                    return Err(OrchestratorInstanceRestoreError::unknown_part(
                        id,
                        part.name.clone(),
                    ));
                }
            }
        }
        Ok(())
    }

    /// Build an agent of `kind` (tasked with `goal`) via the factory and store it,
    /// handing it this instance's [`GraphHost`]. `placement` selects whether
    /// this is the session root or a subagent. The caller owns the
    /// graph/scheduling bookkeeping; this only builds and stores.
    ///
    /// # Errors
    ///
    /// Propagates the factory's typed error when the agent cannot be built
    /// (nothing is stored in that case).
    fn build_agent(
        &mut self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        placement: AgentPlacement,
    ) -> Result<(), FsAgentCreateError> {
        let agent = self.factory.create_agent(
            id,
            kind,
            goal,
            placement,
            Arc::clone(&self.host),
            Arc::from([]),
        )?;
        self.state.get_mut().registry.insert(id, agent);
        Ok(())
    }

    /// Deliver a user message to this session's root.
    ///
    /// Builds the root on the first call (the message becomes its goal); delivers
    /// to the existing idle root afterwards. The agent is left ready; call
    /// [`drive_root_turn`](Self::drive_root_turn) to run it.
    ///
    /// # Errors
    ///
    /// Propagates the typed delivery error when the root cannot be built or
    /// cannot accept the message.
    pub(crate) fn deliver(&mut self, text: impl Into<String>) -> Result<(), InstanceDeliverError> {
        match self.state.get().root {
            Some(root) => self
                .deliver_message(root, text)
                .map_err(|source| InstanceDeliverError::Root { root, source }),
            None => {
                let id = self.agent_id_allocator.next();
                let kind = AgentKind::new(ROOT_AGENT_KIND);
                self.build_agent(id, &kind, text.into(), AgentPlacement::Root(self.session))?;
                self.state.get_mut().meta.insert(
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
                self.state.get_mut().root = Some(id);
                self.enqueue(id);
                Ok(())
            }
        }
    }

    /// What kind of automatic work can currently advance.
    pub(crate) fn work(&self) -> InstanceWork {
        if self.has_root_work() || self.has_unprompted_approval() {
            InstanceWork::Root
        } else if self.has_background_work() {
            InstanceWork::Background
        } else {
            InstanceWork::None
        }
    }

    /// Drive the root-visible foreground turn until the root is no longer ready
    /// or in flight. Background subagents may remain in flight after this
    /// returns; they stay on the instance and continue through
    /// [`drive_background_until_root_ready`](Self::drive_background_until_root_ready).
    pub(crate) async fn drive_root_turn(
        &mut self,
        control: &DriveControl,
        events: &EventSink,
    ) -> (DriveOutput, DriveStop) {
        let mut output = DriveOutput::default();
        let mut cancel_requested = false;
        let mut interrupt_requested = false;

        loop {
            if control.take_cancel() {
                cancel_requested = true;
            }
            if control.take_interrupt() {
                interrupt_requested = true;
            }
            let _ = control.take_wake();

            if !cancel_requested && !interrupt_requested {
                self.start_ready_agent_tasks(events);
            }

            if cancel_requested {
                if self.inflight.is_empty() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Cancelled);
                }
            } else if interrupt_requested {
                if !self.inflight.has_root() {
                    control.clear_cancel_hook();
                    return (output, DriveStop::Interrupted);
                }
            } else if !self.has_root_work() && !self.has_unprompted_approval() {
                break;
            }

            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            // Register before awaiting in-flight ticks so a cancel arriving
            // during LLM/tool work aborts the agents that have been taken out of
            // the registry as well as agents still stored there.
            self.set_cancel_hook(control);

            let ticked = self.inflight.next_completed(control).await;
            self.route_ticked_agents(ticked, events);
        }
        control.clear_cancel_hook();
        output.absorb(self.take_next_approval_prompt());
        (output, DriveStop::Quiescent)
    }

    /// Poll background agents until they either make the root ready, run out of
    /// work, or are woken by a newer user command.
    pub(crate) async fn drive_background_until_root_ready(
        &mut self,
        control: &DriveControl,
        events: &EventSink,
    ) -> DriveStop {
        loop {
            if control.take_cancel() {
                control.clear_cancel_hook();
                return DriveStop::Cancelled;
            }
            if control.take_interrupt() {
                control.clear_cancel_hook();
                return DriveStop::Interrupted;
            }
            if control.take_wake() {
                control.clear_cancel_hook();
                return DriveStop::Interrupted;
            }
            if self.has_root_work() || self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return DriveStop::Quiescent;
            }

            self.start_ready_agent_tasks(events);
            if self.has_root_work() || self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return DriveStop::Quiescent;
            }

            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }

            self.set_cancel_hook(control);

            let ticked = self.inflight.next_completed(control).await;
            self.route_ticked_agents(ticked, events);
            if self.has_unprompted_approval() {
                control.clear_cancel_hook();
                return DriveStop::Quiescent;
            }
        }
        control.clear_cancel_hook();
        DriveStop::Quiescent
    }

    /// Drive cancellation cleanup until no queued or in-flight work remains.
    pub(crate) async fn drive_cancelled(
        &mut self,
        control: &DriveControl,
        events: &EventSink,
    ) -> (DriveOutput, DriveStop) {
        let output = DriveOutput::default();
        loop {
            let _ = control.take_cancel();
            let _ = control.take_interrupt();
            let _ = control.take_wake();
            self.start_ready_agent_tasks(events);
            if self.inflight.is_empty() {
                if self.has_ready() {
                    continue;
                }
                break;
            }
            self.set_cancel_hook(control);
            let ticked = self.inflight.next_completed(control).await;
            self.route_ticked_agents(ticked, events);
        }
        control.clear_cancel_hook();
        (output, DriveStop::Cancelled)
    }

    fn set_cancel_hook(&self, control: &DriveControl) {
        let mut abort_handles = self.state.get().registry.abort_handles();
        abort_handles.extend(self.inflight.abort_handles());
        control.set_cancel_hook(move || {
            for handle in &abort_handles {
                handle.abort();
            }
        });
    }

    pub(crate) fn active_approval(&self) -> Option<PendingApproval> {
        let agent = *self.state.get().approval_queue.front()?;
        let pending = self.state.get().parked_approvals.get(&agent)?;
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

    /// Hard-cancel every live agent that currently has work. Used by session
    /// cancellation cleanup so the session does not leave dormant root/subagent
    /// work behind.
    pub(crate) fn cancel_all(&mut self, reason: CancelReason) {
        let agents: Vec<AgentId> = self.state.get().meta.keys().copied().collect();
        for agent_id in agents {
            let Some(agent) = self.state.get_mut().registry.get_mut(agent_id) else {
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
            let Some(agent) = self.state.get().approval_queue.front().copied() else {
                return DriveOutput::default();
            };
            let Some(pending) = self.state.get_mut().parked_approvals.get_mut(&agent) else {
                self.state.get_mut().approval_queue.pop_front();
                continue;
            };
            if pending.prompted {
                return DriveOutput::default();
            }
            let summary = pending.summary.clone();
            pending.prompted = true;
            return DriveOutput::reply(
                self.session,
                format!(
                    "Permission approval needed:\n{summary}\n\nReply with approval or rejection."
                ),
            );
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
        let decision_name: &'static str = (&decision).into();
        self.state
            .get_mut()
            .registry
            .get_mut(agent)
            .ok_or(ApprovalResolutionError::UnknownAgent(agent))?
            .send_command(AgentCommand::ApprovalResult {
                id: approval,
                decision,
            })
            .map_err(ApprovalResolutionError::Command)?;
        tracing::info!(
            name: "approval_resolved",
            approval = %approval,
            decision = decision_name,
        );
        self.enqueue(agent);
        Ok(())
    }

    /// Start every currently-ready agent and retain its future in the session's
    /// in-flight table.
    fn start_ready_agent_tasks(&mut self, events: &EventSink) {
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
            let is_root = ready.is_root;
            let abort = ready.agent.abort_handle();
            let sink = if ready.is_root {
                events.clone()
            } else {
                EventSink::disabled()
            };
            let Some(meta) = self.state.get().meta.get(&id) else {
                continue;
            };
            let span = tracing::info_span!(
                "agent",
                run.agent = %id,
                kind = %meta.kind.as_str(),
                depth = meta.depth as u64,
            );
            self.inflight.spawn(
                id,
                is_root,
                abort,
                Box::pin(tick_agent(ready, sink).instrument(span)),
            );
        }
    }

    /// Reinsert completed agents, apply effects, then route completed outcomes.
    ///
    /// Root-visible replies are emitted immediately so a fast foreground agent is
    /// not hidden behind slower in-flight subagents. Approval prompts are held
    /// until quiescence by `take_next_approval_prompt`.
    fn route_ticked_agents(&mut self, ticked: Vec<TickedAgent>, events: &EventSink) {
        let mut outcomes = Vec::with_capacity(ticked.len());
        for TickedAgent { id, agent, outcome } in ticked {
            if self.state.get().meta.contains_key(&id) {
                self.state.get_mut().registry.insert(id, agent);
                outcomes.push((id, outcome));
            }
        }

        self.apply_effects();
        for (id, outcome) in outcomes {
            if self.state.get().meta.contains_key(&id) {
                let output = self.route_outcome(id, outcome);
                for reply in output.replies {
                    events.emit(SessionEvent::Output { text: reply.text });
                }
            }
        }
        self.inflight.retain_live(&self.state.get().meta);
        self.flush_subagent_result_mailbox();
    }

    fn drain_ready_agents(&mut self) -> Vec<ReadyAgent> {
        let mut ready_agents = Vec::new();
        while let Some(id) = self.pop_ready() {
            if !self.state.get().meta.contains_key(&id) {
                continue;
            }
            let Some(agent) = self.state.get_mut().registry.take(id) else {
                continue;
            };
            ready_agents.push(ReadyAgent {
                id,
                is_root: self.state.get().root == Some(id),
                agent,
            });
        }
        ready_agents
    }

    fn pop_ready(&mut self) -> Option<AgentId> {
        if self.state.get().ready.is_empty() {
            return None;
        }
        self.state.get_mut().ready.pop_front()
    }

    /// Append a user message to the idle agent `id` and mark it ready.
    fn deliver_message(
        &mut self,
        id: AgentId,
        text: impl Into<String>,
    ) -> Result<(), AgentMessageDeliveryError> {
        let Some(agent) = self.state.get_mut().registry.get_mut(id) else {
            return Err(AgentMessageDeliveryError::UnknownAgent(id));
        };
        agent.send_command(AgentCommand::AppendMessage(text.into()))?;
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
        if !self.state.get().meta.contains_key(&agent) {
            return DriveOutput::default();
        }
        self.state.get_mut().parked_approvals.insert(
            agent,
            ParkedApproval {
                approval,
                summary,
                prompted: false,
            },
        );
        if !self.state.get().approval_queue.contains(&agent) {
            self.state.get_mut().approval_queue.push_back(agent);
        }

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
                name: "delete_ignored",
                target_agent = %target,
                reason = "not_descendant",
            );
            return;
        }
        self.delete_subtree(target);
    }

    /// Whether `node` is a strict descendant of `ancestor` (walking parent edges
    /// in `meta`). `false` when `node == ancestor` or the chain never reaches it.
    fn is_descendant(&self, ancestor: AgentId, node: AgentId) -> bool {
        let mut current = self
            .state
            .get()
            .meta
            .get(&node)
            .and_then(|meta| meta.parent);
        while let Some(parent) = current {
            if parent == ancestor {
                return true;
            }
            current = self
                .state
                .get()
                .meta
                .get(&parent)
                .and_then(|meta| meta.parent);
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
        for (&id, meta) in &self.state.get().meta {
            let status = if self.state.get().parked_approvals.contains_key(&id) {
                AgentStatus::AwaitingApproval
            } else if self.state.get().ready.contains(&id) {
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
        let Some(parent_meta) = self.state.get().meta.get(&parent) else {
            tracing::warn!(
                name: "spawn_dropped",
                parent_agent = %parent,
                kind = %kind.as_str(),
                reason = "missing_parent",
            );
            return;
        };
        let depth = parent_meta.depth.saturating_add(1);
        match self.build_agent(id, &kind, goal, AgentPlacement::Sub(id)) {
            Ok(()) => {
                tracing::info!(
                    name: "spawn_materialized",
                    parent_agent = %parent,
                    child_agent = %id,
                    kind = %kind.as_str(),
                );
                self.state.get_mut().meta.insert(
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
                tracing::error!(
                    name: "spawn_dropped",
                    parent_agent = %parent,
                    kind = %kind.as_str(),
                    reason = "build_failed",
                    error = %error,
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
            .state
            .get()
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

        self.deliver_or_mailbox_subagent_result(parent_id, id, text, ok);

        // Keep a `Manual` subagent alive only on an ordinary yield; otherwise remove
        // it (and its subtree, so a persistent grandchild is never left orphaned).
        let keep_alive = termination == TerminationPolicy::Manual && ok && !ended;
        if keep_alive {
            tracing::info!(name: "manual_yielded", "");
        }
        if !keep_alive {
            self.delete_subtree(id);
        }
        Vec::new()
    }

    /// Route a hard cancellation. Cancellation is intentionally silent: no root
    /// reply and no parent message. A cancelled subagent is removed so the graph
    /// does not retain dead work; a cancelled root stays as the reusable session
    /// root.
    fn route_cancelled(&mut self, id: AgentId, _reason: CancelReason) -> DriveOutput {
        if self
            .state
            .get()
            .meta
            .get(&id)
            .and_then(|meta| meta.parent)
            .is_none()
        {
            return DriveOutput::default();
        }
        self.delete_subtree(id);
        DriveOutput::default()
    }

    /// Remove `root` and every descendant from the store, the graph, the ready
    /// queue, and any parked approvals. Used both for one-shot cleanup and for an
    /// explicit/cascading delete; a parent's removal never leaves orphans.
    fn delete_subtree(&mut self, root: AgentId) {
        let victims = self.subtree_ids(root);
        tracing::info!(
            name: "subtree_deleted",
            root_agent = %root,
            count = victims.len() as u64,
        );
        for victim in &victims {
            self.state.get_mut().registry.remove(*victim);
            self.state.get_mut().meta.remove(victim);
            self.state.get_mut().parked_approvals.remove(victim);
        }
        self.state
            .get_mut()
            .ready
            .retain(|queued| !victims.contains(queued));
        self.state
            .get_mut()
            .approval_queue
            .retain(|queued| !victims.contains(queued));
        self.state
            .get_mut()
            .subagent_result_mailbox
            .retain(|result| !victims.contains(&result.parent));
    }

    fn deliver_or_mailbox_subagent_result(
        &mut self,
        parent: AgentId,
        child: AgentId,
        text: String,
        ok: bool,
    ) {
        if !self.state.get().meta.contains_key(&parent) {
            return;
        }
        if self.state.get().parked_approvals.contains_key(&parent) {
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = true,
            );
            self.state
                .get_mut()
                .subagent_result_mailbox
                .push_back(SubagentResult {
                    parent,
                    child,
                    text,
                    ok,
                });
            return;
        }
        if let Some(parent_agent) = self.state.get_mut().registry.get_mut(parent) {
            parent_agent.deliver_child_result(child, text, ok);
            self.enqueue(parent);
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = false,
            );
        } else {
            tracing::info!(
                name: "result_to_parent",
                parent_agent = %parent,
                child_agent = %child,
                queued = true,
            );
            self.state
                .get_mut()
                .subagent_result_mailbox
                .push_back(SubagentResult {
                    parent,
                    child,
                    text,
                    ok,
                });
        }
    }

    fn flush_subagent_result_mailbox(&mut self) {
        if self.state.get().subagent_result_mailbox.is_empty() {
            return;
        }
        let mut pending = VecDeque::new();
        while let Some(result) = self.pop_subagent_result_mailbox() {
            if !self.state.get().meta.contains_key(&result.parent) {
                continue;
            }
            if self
                .state
                .get()
                .parked_approvals
                .contains_key(&result.parent)
            {
                pending.push_back(result);
                continue;
            }
            let parent = result.parent;
            if let Some(parent_agent) = self.state.get_mut().registry.get_mut(parent) {
                parent_agent.deliver_child_result(result.child, result.text, result.ok);
                self.enqueue(parent);
            } else {
                pending.push_back(result);
            }
        }
        self.state.get_mut().subagent_result_mailbox = pending;
    }

    fn pop_subagent_result_mailbox(&mut self) -> Option<SubagentResult> {
        if self.state.get().subagent_result_mailbox.is_empty() {
            return None;
        }
        self.state.get_mut().subagent_result_mailbox.pop_front()
    }

    /// Collect `root` and all of its descendants (a breadth-first walk of the
    /// parent edges in `meta`).
    fn subtree_ids(&self, root: AgentId) -> Vec<AgentId> {
        let mut out = vec![root];
        let mut frontier = VecDeque::from([root]);
        while let Some(current) = frontier.pop_front() {
            for (&id, meta) in &self.state.get().meta {
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
        if !self.state.get().ready.contains(&id) {
            self.state.get_mut().ready.push_back(id);
        }
    }

    /// True while any agent has work queued.
    fn has_ready(&self) -> bool {
        !self.state.get().ready.is_empty()
    }

    fn has_root_work(&self) -> bool {
        let Some(root) = self.state.get().root else {
            return false;
        };
        self.state.get().ready.contains(&root) || self.inflight.has_root()
    }

    fn has_background_work(&self) -> bool {
        let root = self.state.get().root;
        self.state.get().ready.iter().any(|id| Some(*id) != root) || self.inflight.has_background()
    }

    fn has_unprompted_approval(&self) -> bool {
        self.state.get().approval_queue.iter().any(|agent| {
            self.state
                .get()
                .parked_approvals
                .get(agent)
                .is_some_and(|pending| !pending.prompted)
        })
    }

    fn pop_active_approval(&mut self) -> Option<PendingApproval> {
        while let Some(agent) = self.pop_approval_queue() {
            let Some(pending) = self.state.get_mut().parked_approvals.remove(&agent) else {
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

    fn pop_approval_queue(&mut self) -> Option<AgentId> {
        if self.state.get().approval_queue.is_empty() {
            return None;
        }
        self.state.get_mut().approval_queue.pop_front()
    }
}
