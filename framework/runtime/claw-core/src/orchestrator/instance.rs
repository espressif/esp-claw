//! Per-session agent runtime: the **graph + scheduler + lifecycle policy**.
//!
//! Each session owns one [`OrchestratorInstance`]. It holds:
//! - durable graph state — root plus node topology and lifecycle metadata;
//! - durable scheduler state — ready work, approvals, and result mailboxes;
//! - non-durable runtime ownership — the live [`AgentRegistry`] and in-flight
//!   agent tasks, held directly by the instance;
//! - an explicit graph read model shared with inspection tools.
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

mod approval_flow;
mod construction;
mod drive;
mod graph_flow;
mod graph_state;
mod inflight;
mod persistence;
mod scheduler;

use std::rc::Rc;
use std::sync::Arc;

use claw_checkpoint::DurableState;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentIdAllocator, AgentRegistry, FsAgentFactory, GraphHost};
use crate::session::SessionId;

pub(crate) use self::approval_flow::ApprovalResolutionError;
pub(crate) use self::drive::{DriveOutput, TurnStopMode};
pub(crate) use self::graph_flow::InstanceDeliverError;
use self::graph_state::{EffectQueue, GraphState, SnapshotView};
use self::inflight::InflightAgentTasks;
pub(crate) use self::persistence::OrchestratorInstanceRestore;
pub(super) use self::persistence::OrchestratorInstanceRestoreError;
use self::scheduler::SchedulerState;
pub(crate) use self::scheduler::{InstanceWork, PendingApproval};

pub(in crate::orchestrator::instance) const ROOT_AGENT_KIND: &str = "conversation";

#[derive(Default)]
pub(crate) struct OrchestratorInstanceState {
    pub(in crate::orchestrator::instance) graph: GraphState,
    pub(in crate::orchestrator::instance) scheduler: SchedulerState,
}

/// One session's agent store, graph, scheduler, and root.
pub(crate) struct OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    session: SessionId,
    /// Builds agents (root and children). Owned here; the registry only stores.
    factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
    /// Shared, process-wide id allocator for roots and spawned children.
    agent_id_allocator: AgentIdAllocator,
    /// Durable graph and scheduler state.
    state: DurableState<OrchestratorInstanceState>,
    /// Non-durable live agents at rest between ticks.
    registry: AgentRegistry<Http, Timer>,
    /// Non-durable ticks currently in flight. Agents move between this table and
    /// `registry` without becoming part of checkpoint state.
    inflight: InflightAgentTasks<Http, Timer>,
    /// Graph effects emitted by agents during the current/last tick, applied after
    /// it at a borrow-safe point. Shared with every agent's [`GraphHost`].
    effects: EffectQueue,
    /// The read-only graph projection agents' inspection tools read, refreshed at
    /// scheduling boundaries. Shared with every agent's [`GraphHost`].
    snapshots: SnapshotView,
    /// The [`GraphHost`] handed to every agent this instance builds.
    host: Arc<dyn GraphHost>,
}
