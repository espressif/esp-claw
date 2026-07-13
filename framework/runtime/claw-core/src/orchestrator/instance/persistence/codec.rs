use std::borrow::Cow;
use std::collections::{BTreeMap, BTreeSet, VecDeque};

use claw_checkpoint::{DurablePartError, DurableStateCodec, PartStateBlob, PartStateSlice};
use claw_interface::{ClawHttp, ClawTimer};

use crate::agent::{AgentId, AgentKind, AgentRegistry, TerminationPolicy};

use super::super::graph_state::{GraphState, NodeMeta};
use super::super::scheduler::{ParkedApproval, SchedulerState, SubagentResult};
use super::super::OrchestratorInstanceState;
use super::schema::{
    AgentNodeSnapshot, AgentPartState, AgentPartsSnapshot, ApprovalSnapshot,
    OrchestratorInstanceSnapshot, SubagentResultSnapshot,
};
use super::OrchestratorInstanceRestore;

const ORCHESTRATOR_INSTANCE_SCHEMA_VERSION: u32 = 2;

#[derive(Clone, Copy)]
enum AgentPartsMode {
    FullProduct,
    StateOnly,
}

fn checkpoint_snapshot(
    state: &OrchestratorInstanceState,
    agent_parts: Vec<AgentPartsSnapshot>,
    parts_mode: AgentPartsMode,
) -> Result<OrchestratorInstanceSnapshot, DurablePartError> {
    let mut agents = Vec::with_capacity(state.graph.node_count());
    for (id, meta) in state.graph.nodes() {
        agents.push(AgentNodeSnapshot {
            id,
            parent: meta.parent(),
            kind: meta.kind().as_str().to_string(),
            name: meta.name().map(str::to_owned),
            termination_policy: {
                let label: &'static str = meta.termination().into();
                label.to_string()
            },
        });
    }
    let approvals = state
        .scheduler
        .approvals()
        .map(|(agent, pending)| ApprovalSnapshot {
            agent,
            summary: pending.summary.clone(),
            prompted: pending.prompted,
        })
        .collect();

    let snapshot = OrchestratorInstanceSnapshot {
        agents,
        ready_queue: state.scheduler.ready_ids().collect(),
        approvals,
        subagent_result_mailbox: state
            .scheduler
            .subagent_results()
            .map(|result| SubagentResultSnapshot {
                parent: result.parent,
                child: result.child,
                text: result.text.clone(),
                ok: result.ok,
            })
            .collect(),
        agent_parts,
    };
    validate_snapshot(&snapshot, parts_mode)?;
    Ok(snapshot)
}

fn checkpoint_agent_parts<Http: ClawHttp, Timer: ClawTimer>(
    registry: &AgentRegistry<Http, Timer>,
) -> Result<Vec<AgentPartsSnapshot>, DurablePartError> {
    registry
        .iter()
        .map(|(id, agent)| {
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
            Ok(AgentPartsSnapshot { id, parts })
        })
        .collect()
}

pub(super) fn encode_checkpoint<Http: ClawHttp, Timer: ClawTimer>(
    state: &OrchestratorInstanceState,
    registry: &AgentRegistry<Http, Timer>,
) -> Result<PartStateBlob<'static>, DurablePartError> {
    encode_snapshot(checkpoint_snapshot(
        state,
        checkpoint_agent_parts(registry)?,
        AgentPartsMode::FullProduct,
    )?)
}

fn encode_snapshot(
    snapshot: OrchestratorInstanceSnapshot,
) -> Result<PartStateBlob<'static>, DurablePartError> {
    let bytes = serde_json::to_vec(&snapshot).map_err(DurablePartError::Encode)?;
    Ok(PartStateBlob {
        schema_version: ORCHESTRATOR_INSTANCE_SCHEMA_VERSION,
        bytes: Cow::Owned(bytes),
    })
}

impl DurableStateCodec for OrchestratorInstanceState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        encode_snapshot(checkpoint_snapshot(
            self,
            Vec::new(),
            AgentPartsMode::StateOnly,
        )?)
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(decode_restore(state, AgentPartsMode::StateOnly)?.state)
    }
}

impl OrchestratorInstanceRestore {
    pub(crate) fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        decode_restore(state, AgentPartsMode::FullProduct)
    }
}

fn decode_restore(
    state: PartStateSlice<'_>,
    parts_mode: AgentPartsMode,
) -> Result<OrchestratorInstanceRestore, DurablePartError> {
    if state.schema_version != ORCHESTRATOR_INSTANCE_SCHEMA_VERSION {
        return Err(DurablePartError::InvalidState(
            "unsupported orchestrator instance schema version",
        ));
    }
    let snapshot: OrchestratorInstanceSnapshot =
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)?;
    validate_snapshot(&snapshot, parts_mode)?;
    let OrchestratorInstanceSnapshot {
        agents,
        ready_queue,
        approvals,
        subagent_result_mailbox,
        agent_parts,
    } = snapshot;
    let mut nodes = BTreeMap::new();
    for agent in agents {
        nodes.insert(
            agent.id,
            NodeMeta::new(
                agent.parent,
                AgentKind::new(agent.kind),
                agent.name,
                TerminationPolicy::try_from(agent.termination_policy.as_str())
                    .map_err(|_| DurablePartError::InvalidState("unknown termination policy"))?,
            ),
        );
    }
    let approvals = approvals
        .into_iter()
        .map(|approval| {
            (
                approval.agent,
                ParkedApproval {
                    summary: approval.summary,
                    prompted: approval.prompted,
                },
            )
        })
        .collect::<VecDeque<_>>();
    let subagent_result_mailbox = subagent_result_mailbox
        .into_iter()
        .map(|result| SubagentResult {
            parent: result.parent,
            child: result.child,
            text: result.text,
            ok: result.ok,
        })
        .collect();
    let mut pending_agent_parts = BTreeMap::new();
    for agent in agent_parts {
        if pending_agent_parts.insert(agent.id, agent.parts).is_some() {
            return Err(DurablePartError::InvalidState(
                "duplicate agent parts entry",
            ));
        }
    }
    Ok(OrchestratorInstanceRestore {
        state: OrchestratorInstanceState {
            graph: GraphState::restored(nodes),
            scheduler: SchedulerState::restored(
                ready_queue.into(),
                approvals,
                subagent_result_mailbox,
            ),
        },
        agent_parts: pending_agent_parts,
    })
}

fn validate_snapshot(
    snapshot: &OrchestratorInstanceSnapshot,
    parts_mode: AgentPartsMode,
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

    for result in &snapshot.subagent_result_mailbox {
        if !parents.contains_key(&result.parent) || !parents.contains_key(&result.child) {
            return Err(DurablePartError::InvalidState(
                "subagent result references a missing graph agent",
            ));
        }
        if parents.get(&result.child).copied().flatten() != Some(result.parent) {
            return Err(DurablePartError::InvalidState(
                "subagent result parent does not match graph",
            ));
        }
    }

    if matches!(parts_mode, AgentPartsMode::StateOnly) {
        if snapshot.agent_parts.is_empty() {
            return Ok(());
        }
        return Err(DurablePartError::InvalidState(
            "state-only snapshot contains agent parts",
        ));
    }

    let mut part_agents = BTreeSet::new();
    for agent in &snapshot.agent_parts {
        if !parents.contains_key(&agent.id) {
            return Err(DurablePartError::InvalidState(
                "agent parts id is missing from graph",
            ));
        }
        if !part_agents.insert(agent.id) {
            return Err(DurablePartError::InvalidState(
                "duplicate agent parts entry",
            ));
        }
        let mut names = BTreeSet::new();
        for part in &agent.parts {
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
    if part_agents.len() != parents.len()
        || parents.keys().any(|agent| !part_agents.contains(agent))
    {
        return Err(DurablePartError::InvalidState(
            "agent parts do not cover the graph",
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

    use crate::agent::{AgentId, TerminationPolicy};

    use super::super::schema::{
        AgentNodeSnapshot, AgentPartState, AgentPartsSnapshot, ApprovalSnapshot,
        OrchestratorInstanceSnapshot, SubagentResultSnapshot,
    };
    use super::super::OrchestratorInstanceRestore;

    fn node(id: AgentId, parent: Option<AgentId>) -> AgentNodeSnapshot {
        AgentNodeSnapshot {
            id,
            parent,
            kind: "conversation".to_owned(),
            name: None,
            termination_policy: {
                let value: &'static str = TerminationPolicy::AutoOnIdle.into();
                value.to_owned()
            },
        }
    }

    fn snapshot(agents: Vec<AgentNodeSnapshot>) -> OrchestratorInstanceSnapshot {
        let agent_parts = agents
            .iter()
            .map(|agent| required_parts(agent.id))
            .collect();
        OrchestratorInstanceSnapshot {
            agents,
            ready_queue: Vec::new(),
            approvals: Vec::new(),
            subagent_result_mailbox: Vec::new(),
            agent_parts,
        }
    }

    fn required_parts(id: AgentId) -> AgentPartsSnapshot {
        AgentPartsSnapshot {
            id,
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
        snapshot: &OrchestratorInstanceSnapshot,
    ) -> Result<OrchestratorInstanceRestore, claw_checkpoint::DurablePartError> {
        decode_schema(snapshot, 2)
    }

    fn decode_schema(
        snapshot: &OrchestratorInstanceSnapshot,
        schema_version: u32,
    ) -> Result<OrchestratorInstanceRestore, claw_checkpoint::DurablePartError> {
        let bytes = serde_json::to_vec(snapshot).expect("snapshot encodes");
        OrchestratorInstanceRestore::decode_state(PartStateSlice {
            schema_version,
            bytes: &bytes,
        })
    }

    #[test]
    fn schema_two_restore_keeps_agent_parts_outside_durable_state() {
        let root = AgentId(1);
        let mut snapshot = snapshot(vec![node(root, None)]);
        snapshot.ready_queue.push(root);
        snapshot.agent_parts[0].parts[0].bytes = vec![1, 2, 3];
        let restored = decode(&snapshot).expect("snapshot restores");

        assert_eq!(restored.state.graph.root(), Some(root));
        assert!(restored.state.scheduler.is_ready(root));
        assert_eq!(
            restored
                .agent_parts
                .get(&root)
                .and_then(|parts| parts.first())
                .map(|part| part.bytes.as_slice()),
            Some([1, 2, 3].as_slice())
        );
    }

    #[test]
    fn restore_accepts_only_schema_two() {
        let root = AgentId(1);
        let snapshot = snapshot(vec![node(root, None)]);

        assert!(decode_schema(&snapshot, 1).is_err());
        assert!(decode_schema(&snapshot, 3).is_err());
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
            prompted: false,
        });
        assert!(decode(&unknown_approval).is_err());

        let mut duplicate_approval = snapshot(vec![node(root, None)]);
        duplicate_approval.approvals = vec![
            ApprovalSnapshot {
                agent: root,
                summary: "first".to_owned(),
                prompted: false,
            },
            ApprovalSnapshot {
                agent: root,
                summary: "second".to_owned(),
                prompted: false,
            },
        ];
        assert!(decode(&duplicate_approval).is_err());

        let mut ready_and_approval = snapshot(vec![node(root, None)]);
        ready_and_approval.ready_queue.push(root);
        ready_and_approval.approvals.push(ApprovalSnapshot {
            agent: root,
            summary: "permission".to_owned(),
            prompted: false,
        });
        assert!(decode(&ready_and_approval).is_err());
    }

    #[test]
    fn restore_rejects_invalid_subagent_result_relationships() {
        let root = AgentId(1);
        let child = AgentId(2);
        let unrelated = AgentId(3);
        let agents = || {
            vec![
                node(root, None),
                node(child, Some(root)),
                node(unrelated, Some(root)),
            ]
        };

        let mut unknown = snapshot(agents());
        unknown
            .subagent_result_mailbox
            .push(SubagentResultSnapshot {
                parent: root,
                child: AgentId(99),
                text: "result".to_owned(),
                ok: true,
            });
        assert!(decode(&unknown).is_err());

        let mut wrong_parent = snapshot(agents());
        wrong_parent
            .subagent_result_mailbox
            .push(SubagentResultSnapshot {
                parent: child,
                child: unrelated,
                text: "result".to_owned(),
                ok: true,
            });
        assert!(decode(&wrong_parent).is_err());
    }

    #[test]
    fn codec_validates_agent_part_envelopes_without_owning_the_agent_roster() {
        let root = AgentId(1);

        let mut unknown = snapshot(vec![node(root, None)]);
        unknown.agent_parts.push(required_parts(AgentId(99)));
        assert!(decode(&unknown).is_err());

        let mut duplicate_agent = snapshot(vec![node(root, None)]);
        duplicate_agent.agent_parts.push(required_parts(root));
        assert!(decode(&duplicate_agent).is_err());

        let mut duplicate_name = snapshot(vec![node(root, None)]);
        duplicate_name.agent_parts[0].parts = vec![part("base-agent"), part("base-agent")];
        assert!(decode(&duplicate_name).is_err());

        let mut missing_entry = snapshot(vec![node(root, None)]);
        missing_entry.agent_parts.clear();
        assert!(decode(&missing_entry).is_err());

        let mut missing_required_part = snapshot(vec![node(root, None)]);
        missing_required_part.agent_parts[0].parts.pop();
        assert!(decode(&missing_required_part).is_ok());

        let mut extra_part = snapshot(vec![node(root, None)]);
        extra_part.agent_parts[0].parts.push(part("future-part"));
        assert!(decode(&extra_part).is_ok());
    }

    #[test]
    fn state_only_codec_requires_empty_parts_without_weakening_product_restore() {
        let root = AgentId(1);
        let mut state_only = snapshot(vec![node(root, None)]);
        state_only.agent_parts.clear();
        let bytes = serde_json::to_vec(&state_only).expect("state-only snapshot encodes");
        let slice = PartStateSlice {
            schema_version: 2,
            bytes: &bytes,
        };

        assert!(
            crate::orchestrator::instance::OrchestratorInstanceState::decode_state(slice).is_ok()
        );
        assert!(OrchestratorInstanceRestore::decode_state(slice).is_err());

        let full = snapshot(vec![node(root, None)]);
        let bytes = serde_json::to_vec(&full).expect("full snapshot encodes");
        assert!(
            crate::orchestrator::instance::OrchestratorInstanceState::decode_state(
                PartStateSlice {
                    schema_version: 2,
                    bytes: &bytes,
                }
            )
            .is_err()
        );
    }

    #[test]
    fn schema_two_round_trip_has_one_graph_and_approval_truth() {
        let root = AgentId(1);
        let child = AgentId(2);
        let mut snapshot = snapshot(vec![node(root, None), node(child, Some(root))]);
        snapshot.approvals = vec![
            ApprovalSnapshot {
                agent: child,
                summary: "child permission".to_owned(),
                prompted: false,
            },
            ApprovalSnapshot {
                agent: root,
                summary: "root permission".to_owned(),
                prompted: true,
            },
        ];

        let restored = decode(&snapshot).expect("current snapshot restores");
        assert_eq!(
            restored
                .state
                .scheduler
                .active_approval()
                .map(|pending| pending.agent),
            Some(child)
        );

        let encoded = restored.state.encode_state().expect("state re-encodes");
        assert_eq!(encoded.schema_version, 2);
        let value: serde_json::Value =
            serde_json::from_slice(encoded.bytes.as_ref()).expect("encoded snapshot decodes");
        assert!(value.get("root").is_none());
        assert!(value["agents"][0].get("depth").is_none());
        assert!(value.get("parked_approvals").is_none());
        assert!(value.get("approval_queue").is_none());
        assert_eq!(value["approvals"][0]["agent"], "agent-2");
        assert_eq!(value["approvals"][1]["agent"], "agent-1");
    }

    #[test]
    fn restore_requires_agent_parts_field() {
        let root = AgentId(1);
        let value = serde_json::json!({
            "agents": [serde_json::to_value(node(root, None)).expect("node encodes")],
            "ready_queue": [],
            "approvals": [],
            "subagent_result_mailbox": []
        });
        let bytes = serde_json::to_vec(&value).expect("snapshot value encodes");

        assert!(OrchestratorInstanceRestore::decode_state(PartStateSlice {
            schema_version: 2,
            bytes: &bytes,
        })
        .is_err());
    }
}
