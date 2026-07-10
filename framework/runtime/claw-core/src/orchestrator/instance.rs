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

mod approval_flow;
mod construction;
mod drive;
mod graph_flow;
mod inflight;
mod model;
mod persistence;

use std::sync::Arc;

use claw_checkpoint::DurableState;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentIdAllocator, FsAgentFactory, GraphHost};
use crate::session::SessionId;

use self::inflight::InflightAgentTasks;
pub(crate) use self::model::{
    ApprovalResolutionError, DriveOutput, InstanceDeliverError, OrchestratorInstanceState,
    PendingApproval, RootReply,
};
use self::model::{EffectQueue, SnapshotView};
pub use self::persistence::OrchestratorInstanceRestoreError;

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
