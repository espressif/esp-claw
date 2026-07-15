//! Per-session agent runtime: the **graph + scheduler + lifecycle**.
//!
//! Each session owns one [`MultiagentRuntime`]. It holds:
//! - durable graph state — root plus node topology and lifecycle metadata;
//! - durable scheduler state — ready work and approvals;
//! - stable agent slots — each slot owns its idle agent or its running tick;
//! - one subagent host that owns tool commands and the inspection read model.
//!
//! Responsibility line: the instance decides *when* agents run, *what* their tick
//! outcomes mean (bubble a subagent result to its parent vs. surface a root
//! reply), and *what happens to their lifetimes*. Agent slots only store; agents
//! only compute.
//!
//! Sessions are isolated — one session's agents never appear in another's store —
//! while a single global id allocator (shared at construction) keeps every
//! [`AgentId`] unique across the whole process. The root is built lazily on the
//! first delivered message (that message is its goal); later messages are
//! accepted only once the root has returned to an idle boundary.
//!
//! Borrow safety: orchestrator-owned multiagent tools submit semantic commands
//! through their private port. The instance starts ready agents as owned
//! futures, then — with no agent borrowed — drains and applies those commands
//! before routing completed outcomes.

mod agent_control;
mod agents;
mod approval;
mod construction;
mod drive;
mod drive_control;
mod id_allocator;
mod lifecycle;
mod model;
mod pending_deliveries;
mod persistence;
mod policy;
mod state;
mod timeouts;
mod tool_port;
mod tools;

use std::collections::BTreeMap;
use std::rc::Rc;
use std::sync::Arc;

use claw_checkpoint::DurableState;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_permission::PermissionPolicy;

use crate::agent::FsAgentFactory;
use crate::protocol::{AgentId, SessionId, SessionPersistence};

pub(crate) use self::agent_control::MultiagentDeliverError;
use self::agents::AgentSlots;
pub(crate) use self::approval::ApprovalResolutionError;
pub(crate) use self::drive::{DriveOutput, TurnStopMode};
pub(crate) use self::drive_control::{DriveControl, DriveStop};
pub(crate) use self::id_allocator::AgentIdAllocator;
use self::model::SubagentResult;
use self::pending_deliveries::PendingDeliveries;
pub(crate) use self::persistence::MultiagentRestore;
pub(crate) use self::persistence::MultiagentRestoreError;
pub(crate) use self::state::{MultiagentState, MultiagentWork};
use self::timeouts::AgentTimeouts;
use self::tool_port::MultiagentBridge;

pub(in crate::multiagent) const ROOT_AGENT_KIND: &str = "conversation";

/// Graph placement translated into a generic single-agent environment during
/// construction. This orchestration type never enters `crate::agent`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AgentPlacement {
    Root {
        session: SessionId,
        persistence: SessionPersistence,
    },
    Child(AgentId),
}

/// One session's agent store, graph, scheduler, and root.
pub(crate) struct MultiagentRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    session: SessionId,
    /// Builds agents (root and children). Owned here; slots only store.
    factory: Rc<FsAgentFactory<Filesystem, Http, Timer>>,
    /// Session-owned policy propagated unchanged to every built agent.
    permission_policy: Arc<dyn PermissionPolicy>,
    /// Shared, process-wide id allocator for roots and spawned children.
    agent_id_allocator: AgentIdAllocator,
    /// Durable graph and scheduler state.
    state: DurableState<MultiagentState>,
    /// Stable slots for live graph nodes. Each slot owns the agent in both its
    /// idle and running forms; slot inboxes are part of the checkpoint.
    slots: AgentSlots<Http, Timer>,
    /// Process-local deadline futures, one for every durable non-root node.
    timeouts: AgentTimeouts,
    /// One-shot completions for foreground spawns. They are process-local: an
    /// active tool future owns the matching receiver.
    foreground_results: BTreeMap<AgentId, async_channel::Sender<SubagentResult>>,
    /// Inspection tombstones for background results queued in a parent inbox
    /// but not yet activated into that parent's agent context.
    pending_deliveries: PendingDeliveries,
    /// The only boundary exposed to subagent tools. It owns their pending
    /// commands and read-only inspection projection.
    multiagent: Arc<MultiagentBridge>,
}

impl<Filesystem, Http, Timer> MultiagentRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// A checkpoint may only capture slots whose running ticks have returned
    /// their agents. The session actor defers persistence until this boundary.
    pub(crate) fn checkpoint_ready(&self) -> bool {
        !self.slots.has_running()
    }
}
