use std::collections::BTreeMap;

use crate::protocol::AgentId;

use super::model::{SubagentSnapshot, SubagentStatus};
use super::MultiagentState;

struct PendingDelivery {
    parent: AgentId,
    snapshot: SubagentSnapshot,
}

/// Completed background children whose results are queued but have not yet
/// entered their parent's agent context.
///
/// These entries are inspection tombstones, not live graph nodes. Keeping them
/// separate ensures they cannot be scheduled, followed up, timed out, or count
/// as children for structured join.
#[derive(Default)]
pub(super) struct PendingDeliveries {
    entries: BTreeMap<AgentId, PendingDelivery>,
}

impl PendingDeliveries {
    pub(super) fn record(&mut self, state: &MultiagentState, child: AgentId) -> bool {
        let Some(meta) = state.node(child) else {
            return false;
        };
        let Some(parent) = meta.parent() else {
            return false;
        };
        let Some(depth) = state.depth(child) else {
            return false;
        };
        let snapshot = SubagentSnapshot::new(
            child,
            meta.kind().clone(),
            meta.name().map(str::to_owned),
            Some(parent),
            depth,
            SubagentStatus::CompletedPendingDelivery,
        );
        self.entries
            .insert(child, PendingDelivery { parent, snapshot })
            .is_none()
    }

    pub(super) fn clear_for_parent(&mut self, parent: AgentId) {
        self.entries.retain(|_, delivery| delivery.parent != parent);
    }

    pub(super) fn clear_for_removed_parents(&mut self, removed: &[AgentId]) {
        self.entries
            .retain(|_, delivery| !removed.contains(&delivery.parent));
    }

    pub(super) fn clear(&mut self) {
        self.entries.clear();
    }

    pub(super) fn snapshots(&self) -> impl Iterator<Item = SubagentSnapshot> + '_ {
        self.entries
            .values()
            .map(|delivery| delivery.snapshot.clone())
    }
}

#[cfg(test)]
mod tests {
    use super::PendingDeliveries;
    use crate::multiagent::model::SubagentTimeout;
    use crate::multiagent::MultiagentState;
    use crate::protocol::{AgentId, AgentKind};

    fn timeout() -> SubagentTimeout {
        SubagentTimeout::from_millis(60_000).expect("positive timeout")
    }

    #[test]
    fn records_completed_metadata_without_keeping_a_live_graph_node() {
        let root = AgentId(1);
        let child = AgentId(2);
        let mut state = MultiagentState::default();
        assert!(state.insert_root(root, AgentKind::from_static("conversation")));
        assert!(state.insert_child(
            root,
            child,
            AgentKind::from_static("worker"),
            Some("alpha".to_owned()),
            timeout(),
        ));
        let mut pending = PendingDeliveries::default();

        assert!(pending.record(&state, child));
        state.remove_agents(&[child]);

        let snapshots = pending.snapshots().collect::<Vec<_>>();
        assert_eq!(snapshots.len(), 1);
        let value = serde_json::to_value(&snapshots[0]).expect("snapshot serializes");
        assert_eq!(value["agent"], "agent-2");
        assert_eq!(value["name"], "alpha");
        assert_eq!(value["status"], "completed_pending_delivery");
        assert!(!state.contains(child));

        pending.clear_for_parent(root);
        assert_eq!(pending.snapshots().count(), 0);
    }

    #[test]
    fn deleting_an_owner_clears_its_pending_delivery_tombstones() {
        let root = AgentId(1);
        let parent = AgentId(2);
        let child = AgentId(3);
        let mut state = MultiagentState::default();
        assert!(state.insert_root(root, AgentKind::from_static("conversation")));
        assert!(state.insert_child(
            root,
            parent,
            AgentKind::from_static("worker"),
            None,
            timeout(),
        ));
        assert!(state.insert_child(
            parent,
            child,
            AgentKind::from_static("worker"),
            None,
            timeout(),
        ));
        let mut pending = PendingDeliveries::default();
        assert!(pending.record(&state, child));

        pending.clear_for_removed_parents(&[parent]);

        assert_eq!(pending.snapshots().count(), 0);
    }
}
