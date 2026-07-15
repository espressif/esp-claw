use core::num::NonZeroU32;
use core::time::Duration;
use std::collections::{BTreeMap, BTreeSet};

use serde::ser::{SerializeStruct, Serializer};
use serde::Serialize;
use strum::IntoStaticStr;

use crate::protocol::{AgentId, AgentKind, Message};

/// Everything the orchestrator needs to materialize one child agent.
pub(in crate::multiagent) struct SubagentSpec {
    kind: AgentKind,
    name: Option<String>,
    goal: Message,
    timeout: SubagentTimeout,
}

impl SubagentSpec {
    pub(in crate::multiagent) fn new(
        kind: AgentKind,
        name: Option<String>,
        goal: Message,
        timeout: SubagentTimeout,
    ) -> Self {
        Self {
            kind,
            name,
            goal,
            timeout,
        }
    }

    pub(in crate::multiagent) fn into_parts(
        self,
    ) -> (AgentKind, Option<String>, Message, SubagentTimeout) {
        (self.kind, self.name, self.goal, self.timeout)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(in crate::multiagent) struct SubagentTimeout(NonZeroU32);

impl SubagentTimeout {
    pub(in crate::multiagent) const fn new(milliseconds: NonZeroU32) -> Self {
        Self(milliseconds)
    }

    pub(in crate::multiagent) fn from_millis(milliseconds: u32) -> Option<Self> {
        NonZeroU32::new(milliseconds).map(Self)
    }

    pub(in crate::multiagent) const fn millis(self) -> u32 {
        self.0.get()
    }

    pub(in crate::multiagent) fn duration(self) -> Duration {
        Duration::from_millis(u64::from(self.millis()))
    }
}

pub(in crate::multiagent) trait TranscriptText {
    fn text(&self) -> String;
}

pub(in crate::multiagent) struct SubagentResult {
    id: AgentId,
    text: String,
    ok: bool,
}

impl SubagentResult {
    pub(in crate::multiagent) fn new(id: AgentId, text: String, ok: bool) -> Self {
        Self { id, text, ok }
    }

    pub(in crate::multiagent) fn id(&self) -> AgentId {
        self.id
    }

    pub(in crate::multiagent) fn ok(&self) -> bool {
        self.ok
    }
}

impl TranscriptText for SubagentResult {
    fn text(&self) -> String {
        format!(
            "[subagent] id: {}, result: {}, message: {}",
            self.id, self.ok, self.text
        )
    }
}

#[derive(Clone, Copy, Debug, IntoStaticStr, PartialEq, Eq)]
pub(in crate::multiagent) enum SubagentStatus {
    #[strum(serialize = "ready")]
    Ready,
    #[strum(serialize = "awaiting_approval")]
    AwaitingApproval,
    #[strum(serialize = "running")]
    Running,
    #[strum(serialize = "idle")]
    Idle,
    #[strum(serialize = "completed_pending_delivery")]
    CompletedPendingDelivery,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(in crate::multiagent) struct SubagentSnapshot {
    id: AgentId,
    kind: AgentKind,
    name: Option<String>,
    parent: Option<AgentId>,
    depth: u16,
    status: SubagentStatus,
}

impl SubagentSnapshot {
    pub(in crate::multiagent) fn new(
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        parent: Option<AgentId>,
        depth: u16,
        status: SubagentStatus,
    ) -> Self {
        Self {
            id,
            kind,
            name,
            parent,
            depth,
            status,
        }
    }
}

impl Serialize for SubagentSnapshot {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut state = serializer.serialize_struct("SubagentSnapshot", 6)?;
        state.serialize_field("agent", &self.id)?;
        state.serialize_field("kind", self.kind.as_str())?;
        state.serialize_field("name", &self.name)?;
        state.serialize_field("parent", &self.parent)?;
        state.serialize_field("depth", &self.depth)?;
        let status: &'static str = self.status.into();
        state.serialize_field("status", status)?;
        state.end()
    }
}

/// Immutable read model published by the session runtime for tool inspection.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(in crate::multiagent) struct MultiagentSnapshot {
    agents: BTreeMap<AgentId, SubagentSnapshot>,
}

impl MultiagentSnapshot {
    pub(in crate::multiagent) fn new(agents: impl IntoIterator<Item = SubagentSnapshot>) -> Self {
        Self {
            agents: agents
                .into_iter()
                .map(|snapshot| (snapshot.id, snapshot))
                .collect(),
        }
    }

    pub(in crate::multiagent) fn descendants_of(&self, ancestor: AgentId) -> Vec<SubagentSnapshot> {
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

    pub(in crate::multiagent) fn descendant(
        &self,
        ancestor: AgentId,
        target: AgentId,
    ) -> Option<SubagentSnapshot> {
        is_strict_descendant(ancestor, target, |id| {
            self.agents.get(&id).and_then(|node| node.parent)
        })
        .then(|| self.agents.get(&target).cloned())
        .flatten()
    }
}

/// Shared parent-chain rule for durable topology and its read model.
pub(in crate::multiagent) fn is_strict_descendant(
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
    use super::{MultiagentSnapshot, SubagentSnapshot, SubagentStatus};
    use crate::protocol::{AgentId, AgentKind};

    fn snapshot(
        id: AgentId,
        parent: Option<AgentId>,
        depth: u16,
        status: SubagentStatus,
    ) -> SubagentSnapshot {
        SubagentSnapshot::new(
            id,
            AgentKind::from_static("test"),
            None,
            parent,
            depth,
            status,
        )
    }

    #[test]
    fn snapshot_scopes_inspection_to_strict_descendants() {
        let root = AgentId(1);
        let child = AgentId(2);
        let grandchild = AgentId(3);
        let unrelated = AgentId(4);
        let graph = MultiagentSnapshot::new([
            snapshot(root, None, 0, SubagentStatus::Idle),
            snapshot(child, Some(root), 1, SubagentStatus::Running),
            snapshot(grandchild, Some(child), 2, SubagentStatus::Ready),
            snapshot(unrelated, None, 0, SubagentStatus::Idle),
        ]);

        assert_eq!(
            graph
                .descendants_of(root)
                .into_iter()
                .map(|snapshot| snapshot.id)
                .collect::<Vec<_>>(),
            vec![child, grandchild]
        );
        assert!(graph.descendant(root, root).is_none());
        assert!(graph.descendant(root, unrelated).is_none());
        assert_eq!(
            graph.descendant(root, child).map(|node| node.status),
            Some(SubagentStatus::Running)
        );
    }

    #[test]
    fn completed_pending_delivery_status_has_a_stable_wire_name() {
        let value = serde_json::to_value(snapshot(
            AgentId(2),
            Some(AgentId(1)),
            1,
            SubagentStatus::CompletedPendingDelivery,
        ))
        .expect("snapshot serializes");

        assert_eq!(value["status"], "completed_pending_delivery");
    }
}
