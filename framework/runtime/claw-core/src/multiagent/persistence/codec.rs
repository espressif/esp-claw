use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet, VecDeque};

use claw_checkpoint::{DurablePartError, DurableStateCodec, PartStateBlob, PartStateSlice};
use claw_interface::{ClawHttp, ClawTimer};

use crate::protocol::{AgentId, AgentKind};

use super::super::agents::AgentSlots;
use super::super::state::{NodeMeta, ParkedApproval};
use super::super::MultiagentState;
use super::schema::{
    AgentNodeSnapshot, AgentPartState, AgentSlotSnapshot, ApprovalSnapshot, MultiagentCheckpoint,
};
use super::{MultiagentRestore, RestoredAgentSlot};

const MULTIAGENT_SCHEMA_VERSION: u32 = 5;

#[derive(Clone, Copy)]
enum AgentSlotsMode {
    FullProduct,
    StateOnly,
}

fn checkpoint_snapshot(
    state: &MultiagentState,
    agent_slots: Vec<AgentSlotSnapshot>,
    slots_mode: AgentSlotsMode,
) -> Result<MultiagentCheckpoint, DurablePartError> {
    let mut agents = Vec::with_capacity(state.node_count());
    for (id, meta) in state.nodes() {
        agents.push(AgentNodeSnapshot {
            id,
            parent: meta.parent(),
            kind: meta.kind().as_str().to_string(),
            name: meta.name().map(str::to_owned),
        });
    }
    let approvals = state
        .approvals()
        .map(|(agent, pending)| ApprovalSnapshot {
            agent,
            summary: pending.summary.clone(),
        })
        .collect();

    let snapshot = MultiagentCheckpoint {
        agents,
        ready_queue: state.ready_ids().collect(),
        approvals,
        agent_slots,
    };
    validate_snapshot(&snapshot, slots_mode)?;
    Ok(snapshot)
}

fn checkpoint_agent_slots<Http: ClawHttp, Timer: ClawTimer>(
    slots: &AgentSlots<Http, Timer>,
) -> Result<Vec<AgentSlotSnapshot>, DurablePartError> {
    slots
        .views()
        .map(|slot| {
            let agent = slot.agent().ok_or(DurablePartError::InvalidState(
                "cannot checkpoint an in-flight agent",
            ))?;
            let parts = agent
                .durable_parts()
                .into_iter()
                .map(|part| {
                    let state = part.export_state()?;
                    Ok(AgentPartState {
                        name: part.name().to_owned(),
                        schema_version: state.schema_version,
                        bytes: state.bytes.into_owned(),
                    })
                })
                .collect::<Result<Vec<_>, DurablePartError>>()?;
            Ok(AgentSlotSnapshot {
                id: slot.id(),
                inbox: slot.inbox().iter().cloned().collect(),
                parts,
            })
        })
        .collect()
}

pub(super) fn encode_checkpoint<Http: ClawHttp, Timer: ClawTimer>(
    state: &MultiagentState,
    slots: &AgentSlots<Http, Timer>,
) -> Result<PartStateBlob<'static>, DurablePartError> {
    encode_snapshot(checkpoint_snapshot(
        state,
        checkpoint_agent_slots(slots)?,
        AgentSlotsMode::FullProduct,
    )?)
}

fn encode_snapshot(
    snapshot: MultiagentCheckpoint,
) -> Result<PartStateBlob<'static>, DurablePartError> {
    let bytes = serde_json::to_vec(&snapshot).map_err(DurablePartError::Encode)?;
    Ok(PartStateBlob {
        schema_version: MULTIAGENT_SCHEMA_VERSION,
        bytes: Cow::Owned(bytes),
    })
}

impl DurableStateCodec for MultiagentState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        encode_snapshot(checkpoint_snapshot(
            self,
            Vec::new(),
            AgentSlotsMode::StateOnly,
        )?)
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(decode_restore(state, AgentSlotsMode::StateOnly)?.state)
    }
}

impl MultiagentRestore {
    pub(crate) fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        decode_restore(state, AgentSlotsMode::FullProduct)
    }
}

fn decode_restore(
    state: PartStateSlice<'_>,
    slots_mode: AgentSlotsMode,
) -> Result<MultiagentRestore, DurablePartError> {
    if state.schema_version != MULTIAGENT_SCHEMA_VERSION {
        return Err(DurablePartError::InvalidState(
            "unsupported multiagent runtime schema version",
        ));
    }
    let snapshot: MultiagentCheckpoint =
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)?;
    validate_snapshot(&snapshot, slots_mode)?;
    let MultiagentCheckpoint {
        agents,
        ready_queue,
        approvals,
        agent_slots,
    } = snapshot;
    let mut nodes = BTreeMap::new();
    for agent in agents {
        nodes.insert(
            agent.id,
            NodeMeta::new(agent.parent, AgentKind::new(agent.kind), agent.name),
        );
    }
    let approvals = approvals
        .into_iter()
        .map(|approval| {
            (
                approval.agent,
                ParkedApproval {
                    summary: approval.summary,
                },
            )
        })
        .collect::<VecDeque<_>>();
    let mut pending_agent_slots = BTreeMap::new();
    for slot in agent_slots {
        if pending_agent_slots
            .insert(
                slot.id,
                RestoredAgentSlot {
                    inbox: slot.inbox,
                    parts: slot.parts,
                },
            )
            .is_some()
        {
            return Err(DurablePartError::InvalidState("duplicate agent slot entry"));
        }
    }
    Ok(MultiagentRestore {
        state: MultiagentState::restored(nodes, ready_queue.into(), approvals),
        agent_slots: pending_agent_slots,
    })
}

fn validate_snapshot(
    snapshot: &MultiagentCheckpoint,
    slots_mode: AgentSlotsMode,
) -> Result<(), DurablePartError> {
    let mut parents = BTreeMap::new();
    for agent in &snapshot.agents {
        if agent.kind.trim().is_empty() {
            return Err(DurablePartError::InvalidState("agent kind is empty"));
        }
        if parents.insert(agent.id, agent.parent).is_some() {
            return Err(DurablePartError::InvalidState("duplicate graph agent id"));
        }
    }
    validate_topology(&parents)?;

    let mut ready = BTreeSet::new();
    for agent in &snapshot.ready_queue {
        if !parents.contains_key(agent) {
            return Err(DurablePartError::InvalidState(
                "ready agent is missing from graph",
            ));
        }
        if !ready.insert(*agent) {
            return Err(DurablePartError::InvalidState("duplicate ready agent"));
        }
    }

    let mut approval_agents = BTreeSet::new();
    for approval in &snapshot.approvals {
        if !parents.contains_key(&approval.agent) {
            return Err(DurablePartError::InvalidState(
                "approval agent is missing from graph",
            ));
        }
        if !approval_agents.insert(approval.agent) {
            return Err(DurablePartError::InvalidState("duplicate approval agent"));
        }
        if ready.contains(&approval.agent) {
            return Err(DurablePartError::InvalidState(
                "approval agent is also ready",
            ));
        }
    }

    if matches!(slots_mode, AgentSlotsMode::StateOnly) {
        if snapshot.agent_slots.is_empty() {
            return Ok(());
        }
        return Err(DurablePartError::InvalidState(
            "state-only snapshot contains agent slots",
        ));
    }

    let mut slot_agents = BTreeSet::new();
    for slot in &snapshot.agent_slots {
        if !parents.contains_key(&slot.id) {
            return Err(DurablePartError::InvalidState(
                "agent slot id is missing from graph",
            ));
        }
        if !slot_agents.insert(slot.id) {
            return Err(DurablePartError::InvalidState("duplicate agent slot entry"));
        }
        let mut names = BTreeSet::new();
        for part in &slot.parts {
            if part.name.is_empty() {
                return Err(DurablePartError::InvalidState(
                    "agent durable part name is empty",
                ));
            }
            if !names.insert(part.name.as_str()) {
                return Err(DurablePartError::InvalidState(
                    "duplicate agent durable part name",
                ));
            }
        }
    }
    if slot_agents.len() != parents.len()
        || parents.keys().any(|agent| !slot_agents.contains(agent))
    {
        return Err(DurablePartError::InvalidState(
            "agent slots do not cover the graph",
        ));
    }
    Ok(())
}

fn validate_topology(parents: &BTreeMap<AgentId, Option<AgentId>>) -> Result<(), DurablePartError> {
    if parents.is_empty() {
        return Ok(());
    }
    for parent in parents.values().flatten() {
        if !parents.contains_key(parent) {
            return Err(DurablePartError::InvalidState("graph parent is missing"));
        }
    }
    for start in parents.keys().copied() {
        let mut visited = BTreeSet::new();
        let mut current = start;
        loop {
            if !visited.insert(current) {
                return Err(DurablePartError::InvalidState("graph contains a cycle"));
            }
            let Some(parent) = parents.get(&current).copied().flatten() else {
                break;
            };
            current = parent;
        }
    }
    if parents.values().filter(|parent| parent.is_none()).count() != 1 {
        return Err(DurablePartError::InvalidState(
            "graph must contain exactly one root",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use claw_checkpoint::{DurableStateCodec, PartStateSlice};

    use crate::protocol::AgentId;
    use crate::protocol::Message;

    use super::super::schema::{
        AgentNodeSnapshot, AgentPartState, AgentSlotSnapshot, ApprovalSnapshot,
        MultiagentCheckpoint,
    };
    use super::super::MultiagentRestore;
    use super::MULTIAGENT_SCHEMA_VERSION;

    fn node(id: AgentId, parent: Option<AgentId>) -> AgentNodeSnapshot {
        AgentNodeSnapshot {
            id,
            parent,
            kind: "conversation".to_owned(),
            name: None,
        }
    }

    fn snapshot(agents: Vec<AgentNodeSnapshot>) -> MultiagentCheckpoint {
        let agent_slots = agents.iter().map(|agent| required_slot(agent.id)).collect();
        MultiagentCheckpoint {
            agents,
            ready_queue: Vec::new(),
            approvals: Vec::new(),
            agent_slots,
        }
    }

    fn required_slot(id: AgentId) -> AgentSlotSnapshot {
        AgentSlotSnapshot {
            id,
            inbox: Vec::new(),
            parts: vec![part("base-agent"), part("tool-set")],
        }
    }

    fn part(name: &str) -> AgentPartState {
        AgentPartState {
            name: name.to_owned(),
            schema_version: 2,
            bytes: Vec::new(),
        }
    }

    fn decode(
        snapshot: &MultiagentCheckpoint,
    ) -> Result<MultiagentRestore, claw_checkpoint::DurablePartError> {
        decode_schema(snapshot, MULTIAGENT_SCHEMA_VERSION)
    }

    fn decode_schema(
        snapshot: &MultiagentCheckpoint,
        schema_version: u32,
    ) -> Result<MultiagentRestore, claw_checkpoint::DurablePartError> {
        let bytes = serde_json::to_vec(snapshot).expect("snapshot encodes");
        MultiagentRestore::decode_state(PartStateSlice {
            schema_version,
            bytes: &bytes,
        })
    }

    #[test]
    fn schema_five_restore_keeps_agent_slots_outside_durable_state() {
        let root = AgentId(1);
        let mut snapshot = snapshot(vec![node(root, None)]);
        snapshot.ready_queue.push(root);
        snapshot.agent_slots[0].inbox.push(Message::text("pending"));
        snapshot.agent_slots[0].parts[0].bytes = vec![1, 2, 3];
        let restored = decode(&snapshot).expect("snapshot restores");

        assert_eq!(restored.state.root(), Some(root));
        assert!(restored.state.is_ready(root));
        assert_eq!(
            restored
                .agent_slots
                .get(&root)
                .and_then(|slot| slot.parts.first())
                .map(|part| part.bytes.as_slice()),
            Some([1, 2, 3].as_slice())
        );
        assert_eq!(
            restored
                .agent_slots
                .get(&root)
                .map(|slot| slot.inbox.as_slice()),
            Some([Message::text("pending")].as_slice())
        );
    }

    #[test]
    fn restore_accepts_only_the_current_schema() {
        let root = AgentId(1);
        let snapshot = snapshot(vec![node(root, None)]);

        assert!(decode_schema(&snapshot, MULTIAGENT_SCHEMA_VERSION - 1).is_err());
        assert!(decode_schema(&snapshot, MULTIAGENT_SCHEMA_VERSION + 1).is_err());
    }

    #[test]
    fn restore_rejects_duplicate_dangling_cyclic_or_multi_root_graphs() {
        let root = AgentId(1);
        let child = AgentId(2);

        let duplicate = snapshot(vec![node(root, None), node(root, None)]);
        assert!(decode(&duplicate).is_err());

        let dangling = snapshot(vec![node(root, None), node(child, Some(AgentId(99)))]);
        assert!(decode(&dangling).is_err());

        let cycle = snapshot(vec![node(root, Some(child)), node(child, Some(root))]);
        assert!(decode(&cycle).is_err());

        let multiple_roots = snapshot(vec![node(root, None), node(child, None)]);
        assert!(decode(&multiple_roots).is_err());
    }

    #[test]
    fn restore_rejects_unknown_or_duplicate_ready_and_approval_agents() {
        let root = AgentId(1);
        let missing = AgentId(99);
        let mut unknown_ready = snapshot(vec![node(root, None)]);
        unknown_ready.ready_queue.push(missing);
        assert!(decode(&unknown_ready).is_err());

        let mut duplicate_ready = snapshot(vec![node(root, None)]);
        duplicate_ready.ready_queue = vec![root, root];
        assert!(decode(&duplicate_ready).is_err());

        let mut unknown_approval = snapshot(vec![node(root, None)]);
        unknown_approval.approvals.push(ApprovalSnapshot {
            agent: missing,
            summary: "permission".to_owned(),
        });
        assert!(decode(&unknown_approval).is_err());

        let mut duplicate_approval = snapshot(vec![node(root, None)]);
        duplicate_approval.approvals = vec![
            ApprovalSnapshot {
                agent: root,
                summary: "first".to_owned(),
            },
            ApprovalSnapshot {
                agent: root,
                summary: "second".to_owned(),
            },
        ];
        assert!(decode(&duplicate_approval).is_err());

        let mut ready_and_approval = snapshot(vec![node(root, None)]);
        ready_and_approval.ready_queue.push(root);
        ready_and_approval.approvals.push(ApprovalSnapshot {
            agent: root,
            summary: "permission".to_owned(),
        });
        assert!(decode(&ready_and_approval).is_err());
    }

    #[test]
    fn codec_validates_agent_slot_envelopes_without_owning_the_part_roster() {
        let root = AgentId(1);

        let mut unknown = snapshot(vec![node(root, None)]);
        unknown.agent_slots.push(required_slot(AgentId(99)));
        assert!(decode(&unknown).is_err());

        let mut duplicate_agent = snapshot(vec![node(root, None)]);
        duplicate_agent.agent_slots.push(required_slot(root));
        assert!(decode(&duplicate_agent).is_err());

        let mut duplicate_name = snapshot(vec![node(root, None)]);
        duplicate_name.agent_slots[0].parts = vec![part("base-agent"), part("base-agent")];
        assert!(decode(&duplicate_name).is_err());

        let mut missing_entry = snapshot(vec![node(root, None)]);
        missing_entry.agent_slots.clear();
        assert!(decode(&missing_entry).is_err());

        let mut missing_required_part = snapshot(vec![node(root, None)]);
        missing_required_part.agent_slots[0].parts.pop();
        assert!(decode(&missing_required_part).is_ok());

        let mut extra_part = snapshot(vec![node(root, None)]);
        extra_part.agent_slots[0].parts.push(part("future-part"));
        assert!(decode(&extra_part).is_ok());
    }

    #[test]
    fn state_only_codec_requires_empty_slots_without_weakening_product_restore() {
        let root = AgentId(1);
        let mut state_only = snapshot(vec![node(root, None)]);
        state_only.agent_slots.clear();
        let bytes = serde_json::to_vec(&state_only).expect("state-only snapshot encodes");
        let slice = PartStateSlice {
            schema_version: MULTIAGENT_SCHEMA_VERSION,
            bytes: &bytes,
        };

        assert!(crate::multiagent::MultiagentState::decode_state(slice).is_ok());
        assert!(MultiagentRestore::decode_state(slice).is_err());

        let full = snapshot(vec![node(root, None)]);
        let bytes = serde_json::to_vec(&full).expect("full snapshot encodes");
        assert!(
            crate::multiagent::MultiagentState::decode_state(PartStateSlice {
                schema_version: MULTIAGENT_SCHEMA_VERSION,
                bytes: &bytes,
            })
            .is_err()
        );
    }

    #[test]
    fn current_schema_round_trip_has_one_graph_and_approval_truth() {
        let root = AgentId(1);
        let child = AgentId(2);
        let mut snapshot = snapshot(vec![node(root, None), node(child, Some(root))]);
        snapshot.approvals = vec![
            ApprovalSnapshot {
                agent: child,
                summary: "child permission".to_owned(),
            },
            ApprovalSnapshot {
                agent: root,
                summary: "root permission".to_owned(),
            },
        ];

        let restored = decode(&snapshot).expect("current snapshot restores");
        assert_eq!(
            restored.state.active_approval().map(|(agent, _)| agent),
            Some(child)
        );

        let encoded = restored.state.encode_state().expect("state re-encodes");
        assert_eq!(encoded.schema_version, MULTIAGENT_SCHEMA_VERSION);
        let value: serde_json::Value =
            serde_json::from_slice(encoded.bytes.as_ref()).expect("encoded snapshot decodes");
        assert!(value.get("root").is_none());
        assert!(value["agents"][0].get("depth").is_none());
        assert!(value.get("parked_approvals").is_none());
        assert!(value.get("approval_queue").is_none());
        assert_eq!(value["approvals"][0]["agent"], "agent-2");
        assert_eq!(value["approvals"][1]["agent"], "agent-1");
        assert!(value["approvals"][0].get("prompted").is_none());
        assert!(value["approvals"][1].get("prompted").is_none());
    }

    #[test]
    fn restore_requires_agent_slots_field() {
        let root = AgentId(1);
        let value = serde_json::json!({
            "agents": [serde_json::to_value(node(root, None)).expect("node encodes")],
            "ready_queue": [],
            "approvals": []
        });
        let bytes = serde_json::to_vec(&value).expect("snapshot value encodes");

        assert!(MultiagentRestore::decode_state(PartStateSlice {
            schema_version: MULTIAGENT_SCHEMA_VERSION,
            bytes: &bytes,
        })
        .is_err());
    }
}
