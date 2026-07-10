use std::collections::{BTreeMap, HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use crate::agent::{
    AgentCommandError, AgentId, AgentIdAllocator, AgentKind, AgentRegistry, AgentSnapshot,
    ApprovalId, FsAgentCreateError, GraphEffect, GraphHost, TerminationPolicy,
};
use crate::session::SessionId;

use super::persistence::AgentPartState;

pub(super) const ROOT_AGENT_KIND: &str = "conversation";

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PendingApproval {
    pub(crate) agent: AgentId,
    pub(crate) approval: ApprovalId,
    pub(crate) summary: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct ParkedApproval {
    pub(super) approval: ApprovalId,
    pub(super) summary: String,
    pub(super) prompted: bool,
}

#[derive(Clone)]
pub(super) struct NodeMeta {
    pub(super) parent: Option<AgentId>,
    pub(super) depth: u16,
    pub(super) kind: AgentKind,
    pub(super) name: Option<String>,
    pub(super) termination: TerminationPolicy,
}

pub(super) type EffectQueue = Arc<Mutex<VecDeque<(AgentId, GraphEffect)>>>;
pub(super) type SnapshotView = Arc<Mutex<HashMap<AgentId, AgentSnapshot>>>;

pub(super) struct SubagentResult {
    pub(super) parent: AgentId,
    pub(super) child: AgentId,
    pub(super) text: String,
    pub(super) ok: bool,
}

#[derive(Clone)]
pub(super) struct InstanceHost {
    pub(super) agent_id_allocator: AgentIdAllocator,
    pub(super) effects: EffectQueue,
    pub(super) snapshots: SnapshotView,
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

#[derive(Default)]
pub(crate) struct OrchestratorInstanceState {
    pub(super) registry: AgentRegistry,
    pub(super) root: Option<AgentId>,
    pub(super) meta: BTreeMap<AgentId, NodeMeta>,
    pub(super) ready: VecDeque<AgentId>,
    pub(super) parked_approvals: BTreeMap<AgentId, ParkedApproval>,
    pub(super) approval_queue: VecDeque<AgentId>,
    pub(super) subagent_result_mailbox: VecDeque<SubagentResult>,
    pub(super) pending_agent_parts: BTreeMap<AgentId, Vec<AgentPartState>>,
}

/// A user-facing reply produced by a root agent, surfaced to the channel router.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct RootReply {
    pub session: SessionId,
    pub text: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct DriveOutput {
    pub replies: Vec<RootReply>,
}

impl DriveOutput {
    pub(crate) fn absorb(&mut self, other: DriveOutput) {
        self.replies.extend(other.replies);
    }

    pub(super) fn replies(replies: Vec<RootReply>) -> Self {
        Self { replies }
    }

    pub(super) fn reply(session: SessionId, text: String) -> Self {
        Self {
            replies: vec![RootReply { session, text }],
        }
    }
}

#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum ApprovalResolutionError {
    #[error("no active approval to resolve")]
    NoActiveApproval,
    #[error("no agent {0} to resolve approval for")]
    UnknownAgent(AgentId),
    #[error(transparent)]
    Command(AgentCommandError),
}

#[derive(Debug, thiserror::Error)]
pub(crate) enum InstanceDeliverError {
    #[error("failed to build root agent: {0}")]
    Create(#[from] FsAgentCreateError),
    #[error("failed to deliver to root {root}: {source}")]
    Root {
        root: AgentId,
        #[source]
        source: AgentMessageDeliveryError,
    },
}

#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentMessageDeliveryError {
    #[error("no such agent: {0}")]
    UnknownAgent(AgentId),
    #[error(transparent)]
    Command(#[from] AgentCommandError),
}
