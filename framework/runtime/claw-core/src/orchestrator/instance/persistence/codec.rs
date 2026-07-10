use std::borrow::Cow;

use claw_checkpoint::{DurablePartError, DurableStateCodec, PartStateBlob, PartStateSlice};
use serde::Serialize;

use crate::agent::{AgentKind, AgentRegistry, TerminationPolicy};

use super::super::model::{NodeMeta, OrchestratorInstanceState, ParkedApproval, SubagentResult};
use super::schema::{
    AgentNodeSnapshot, AgentPartState, AgentPartsSnapshot, OrchestratorInstanceSnapshot,
    ParkedApprovalSnapshot, SubagentResultSnapshot,
};

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
                termination_policy: {
                    let label: &'static str = meta.termination.into();
                    label.to_string()
                },
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
        let mut meta = std::collections::BTreeMap::new();
        for agent in snapshot.agents {
            meta.insert(
                agent.id,
                NodeMeta {
                    parent: agent.parent,
                    depth: agent.depth,
                    kind: AgentKind::new(agent.kind),
                    name: agent.name,
                    termination: TerminationPolicy::try_from(agent.termination_policy.as_str())
                        .map_err(|_| {
                            DurablePartError::InvalidState("unknown termination policy")
                        })?,
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
