use claw_checkpoint::SchemaVersion;
use serde::{Deserialize, Serialize};

use crate::protocol::{AgentId, Message};

#[derive(Deserialize, Serialize)]
pub(super) struct MultiagentCheckpoint {
    pub(super) agents: Vec<AgentNodeSnapshot>,
    pub(super) ready_queue: Vec<AgentId>,
    pub(super) approvals: Vec<ApprovalSnapshot>,
    pub(super) agent_slots: Vec<AgentSlotSnapshot>,
}

#[derive(Deserialize, Serialize)]
pub(super) struct AgentNodeSnapshot {
    pub(super) id: AgentId,
    pub(super) parent: Option<AgentId>,
    pub(super) kind: String,
    pub(super) name: Option<String>,
    pub(super) timeout_ms: Option<u32>,
}

#[derive(Deserialize, Serialize)]
pub(super) struct ApprovalSnapshot {
    pub(super) agent: AgentId,
    pub(super) summary: String,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(super) struct AgentSlotSnapshot {
    pub(super) id: AgentId,
    pub(super) inbox: Vec<Message>,
    pub(super) parts: Vec<AgentPartState>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
pub(in crate::multiagent) struct AgentPartState {
    pub(in crate::multiagent) name: String,
    pub(in crate::multiagent) schema_version: SchemaVersion,
    pub(in crate::multiagent) bytes: Vec<u8>,
}
