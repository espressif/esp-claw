use claw_checkpoint::SchemaVersion;
use serde::{Deserialize, Serialize};

use crate::agent::AgentId;

#[derive(Deserialize, Serialize)]
pub(super) struct OrchestratorInstanceSnapshot {
    pub(super) agents: Vec<AgentNodeSnapshot>,
    pub(super) ready_queue: Vec<AgentId>,
    pub(super) approvals: Vec<ApprovalSnapshot>,
    pub(super) subagent_result_mailbox: Vec<SubagentResultSnapshot>,
    pub(super) agent_parts: Vec<AgentPartsSnapshot>,
}

#[derive(Deserialize, Serialize)]
pub(super) struct AgentNodeSnapshot {
    pub(super) id: AgentId,
    pub(super) parent: Option<AgentId>,
    pub(super) kind: String,
    pub(super) name: Option<String>,
    pub(super) termination_policy: String,
}

#[derive(Deserialize, Serialize)]
pub(super) struct ApprovalSnapshot {
    pub(super) agent: AgentId,
    pub(super) summary: String,
    pub(super) prompted: bool,
}

#[derive(Deserialize, Serialize)]
pub(super) struct SubagentResultSnapshot {
    pub(super) parent: AgentId,
    pub(super) child: AgentId,
    pub(super) text: String,
    pub(super) ok: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(super) struct AgentPartsSnapshot {
    pub(super) id: AgentId,
    pub(super) parts: Vec<AgentPartState>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(in crate::orchestrator::instance) struct AgentPartState {
    pub(in crate::orchestrator::instance) name: String,
    pub(in crate::orchestrator::instance) schema_version: SchemaVersion,
    pub(in crate::orchestrator::instance) bytes: Vec<u8>,
}
