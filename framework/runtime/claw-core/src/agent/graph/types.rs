use std::collections::{BTreeMap, BTreeSet};

use serde::ser::{SerializeStruct, Serializer};
use serde::Serialize;
use strum::{EnumString, IntoStaticStr};

use crate::agent::base_agent::AgentId;
use crate::agent::kind::AgentKind;
use crate::session::Message;

#[derive(Clone, Copy, Debug, EnumString, IntoStaticStr, PartialEq, Eq)]
#[strum(
    parse_err_ty = ParseTerminationPolicyError,
    parse_err_fn = ParseTerminationPolicyError::new
)]
pub(crate) enum TerminationPolicy {
    #[strum(serialize = "auto")]
    AutoOnIdle,
    #[strum(serialize = "manual")]
    Manual,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
#[error("unknown termination policy; expected auto or manual")]
pub(crate) struct ParseTerminationPolicyError;

impl ParseTerminationPolicyError {
    fn new(_: &str) -> Self {
        Self
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum GraphEffect {
    Spawn {
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        goal: Message,
        termination: TerminationPolicy,
    },
    Delete {
        target: AgentId,
    },
    Followup {
        target: AgentId,
        message: Message,
    },
}

#[derive(Clone, Copy, Debug, IntoStaticStr, PartialEq, Eq)]
pub(crate) enum AgentStatus {
    #[strum(serialize = "ready")]
    Ready,
    #[strum(serialize = "awaiting_approval")]
    AwaitingApproval,
    #[strum(serialize = "running")]
    Running,
    #[strum(serialize = "idle")]
    Idle,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct AgentSnapshot {
    pub id: AgentId,
    pub kind: AgentKind,
    pub name: Option<String>,
    pub parent: Option<AgentId>,
    pub depth: u16,
    pub termination: TerminationPolicy,
    pub status: AgentStatus,
}

impl Serialize for AgentSnapshot {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut state = serializer.serialize_struct("AgentSnapshot", 7)?;
        state.serialize_field("agent", &self.id)?;
        state.serialize_field("kind", self.kind.as_str())?;
        state.serialize_field("name", &self.name)?;
        state.serialize_field("parent", &self.parent)?;
        state.serialize_field("depth", &self.depth)?;
        let status: &'static str = self.status.into();
        state.serialize_field("status", status)?;
        let termination: &'static str = self.termination.into();
        state.serialize_field("termination", termination)?;
        state.end()
    }
}

/// Immutable graph projection shared with agent inspection tools.
///
/// This is a read model: the orchestrator derives it from its graph, scheduler,
/// and runtime owners. It never becomes another mutable source of truth.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct AgentGraphSnapshot {
    agents: BTreeMap<AgentId, AgentSnapshot>,
}

impl AgentGraphSnapshot {
    pub(crate) fn new(agents: impl IntoIterator<Item = AgentSnapshot>) -> Self {
        Self {
            agents: agents
                .into_iter()
                .map(|snapshot| (snapshot.id, snapshot))
                .collect(),
        }
    }

    pub(crate) fn descendants_of(&self, ancestor: AgentId) -> Vec<AgentSnapshot> {
        let mut descendants = self
            .agents
            .values()
            .filter(|snapshot| {
                is_strict_descendant(ancestor, snapshot.id, |id| {
                    self.agents.get(&id).and_then(|node| node.parent)
                })
            })
            .cloned()
            .collect::<Vec<_>>();
        descendants.sort_by_key(|snapshot| (snapshot.depth, snapshot.id.0));
        descendants
    }

    pub(crate) fn descendant(&self, ancestor: AgentId, target: AgentId) -> Option<AgentSnapshot> {
        is_strict_descendant(ancestor, target, |id| {
            self.agents.get(&id).and_then(|node| node.parent)
        })
        .then(|| self.agents.get(&target).cloned())
        .flatten()
    }
}

/// The single parent-chain rule used by both the durable graph and its read
/// model. Cycles in corrupt input terminate instead of looping forever.
pub(crate) fn is_strict_descendant(
    ancestor: AgentId,
    node: AgentId,
    mut parent_of: impl FnMut(AgentId) -> Option<AgentId>,
) -> bool {
    if ancestor == node {
        return false;
    }
    let mut seen = BTreeSet::new();
    let mut current = parent_of(node);
    while let Some(parent) = current {
        if parent == ancestor {
            return true;
        }
        if !seen.insert(parent) {
            return false;
        }
        current = parent_of(parent);
    }
    false
}

#[cfg(test)]
mod tests {
    use super::{AgentGraphSnapshot, AgentSnapshot, AgentStatus, TerminationPolicy};
    use crate::agent::{AgentId, AgentKind};

    fn snapshot(
        id: AgentId,
        parent: Option<AgentId>,
        depth: u16,
        status: AgentStatus,
    ) -> AgentSnapshot {
        AgentSnapshot {
            id,
            kind: AgentKind::from_static("test"),
            name: None,
            parent,
            depth,
            termination: TerminationPolicy::AutoOnIdle,
            status,
        }
    }

    #[test]
    fn graph_snapshot_is_the_read_model_for_status_and_descendants() {
        let root = AgentId(1);
        let child = AgentId(2);
        let grandchild = AgentId(3);
        let unrelated = AgentId(4);
        let graph = AgentGraphSnapshot::new([
            snapshot(root, None, 0, AgentStatus::Idle),
            snapshot(child, Some(root), 1, AgentStatus::Running),
            snapshot(grandchild, Some(child), 2, AgentStatus::Ready),
            snapshot(unrelated, None, 0, AgentStatus::Idle),
        ]);

        assert_eq!(
            graph
                .descendants_of(root)
                .into_iter()
                .map(|snapshot| snapshot.id)
                .collect::<Vec<_>>(),
            vec![child, grandchild]
        );
        assert_eq!(
            graph.descendant(root, grandchild).map(|node| node.id),
            Some(grandchild)
        );
        assert!(graph.descendant(root, root).is_none());
        assert!(graph.descendant(root, unrelated).is_none());
        assert_eq!(
            graph.descendant(root, child).map(|node| node.status),
            Some(AgentStatus::Running)
        );
    }
}
