use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::sync::{Arc, Mutex};

use crate::agent::{
    is_strict_descendant, AgentGraphSnapshot, AgentId, AgentIdAllocator, AgentKind, AgentSnapshot,
    GraphEffect, GraphHost, TerminationPolicy,
};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use super::OrchestratorInstance;

#[derive(Clone)]
pub(super) struct NodeMeta {
    parent: Option<AgentId>,
    kind: AgentKind,
    name: Option<String>,
    termination: TerminationPolicy,
}

impl NodeMeta {
    pub(super) fn new(
        parent: Option<AgentId>,
        kind: AgentKind,
        name: Option<String>,
        termination: TerminationPolicy,
    ) -> Self {
        Self {
            parent,
            kind,
            name,
            termination,
        }
    }

    pub(super) fn parent(&self) -> Option<AgentId> {
        self.parent
    }

    pub(super) fn kind(&self) -> &AgentKind {
        &self.kind
    }

    pub(super) fn name(&self) -> Option<&str> {
        self.name.as_deref()
    }

    pub(super) fn termination(&self) -> TerminationPolicy {
        self.termination
    }
}

pub(super) type EffectQueue = Arc<Mutex<VecDeque<(AgentId, GraphEffect)>>>;
pub(super) type SnapshotView = Arc<Mutex<AgentGraphSnapshot>>;

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

    fn snapshot(&self) -> AgentGraphSnapshot {
        self.snapshots
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .clone()
    }
}

#[derive(Default)]
pub(super) struct GraphState {
    nodes: BTreeMap<AgentId, NodeMeta>,
}

impl GraphState {
    pub(super) fn restored(nodes: BTreeMap<AgentId, NodeMeta>) -> Self {
        Self { nodes }
    }

    pub(super) fn root(&self) -> Option<AgentId> {
        let mut roots = self
            .nodes
            .iter()
            .filter_map(|(&id, meta)| meta.parent.is_none().then_some(id));
        let root = roots.next()?;
        roots.next().is_none().then_some(root)
    }

    pub(super) fn is_root(&self, id: AgentId) -> bool {
        self.root() == Some(id)
    }

    pub(super) fn contains(&self, id: AgentId) -> bool {
        self.nodes.contains_key(&id)
    }

    pub(super) fn node(&self, id: AgentId) -> Option<&NodeMeta> {
        self.nodes.get(&id)
    }

    pub(super) fn nodes(&self) -> impl Iterator<Item = (AgentId, &NodeMeta)> {
        self.nodes.iter().map(|(&id, meta)| (id, meta))
    }

    pub(super) fn node_count(&self) -> usize {
        self.nodes.len()
    }

    pub(super) fn agent_ids(&self) -> impl Iterator<Item = AgentId> + '_ {
        self.nodes.keys().copied()
    }

    #[must_use]
    pub(super) fn insert_root(&mut self, id: AgentId, kind: AgentKind) -> bool {
        if self.root().is_some() || self.nodes.contains_key(&id) {
            return false;
        }
        self.nodes.insert(
            id,
            NodeMeta::new(None, kind, None, TerminationPolicy::AutoOnIdle),
        );
        true
    }

    pub(super) fn insert_child(
        &mut self,
        parent: AgentId,
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        termination: TerminationPolicy,
    ) -> bool {
        if !self.nodes.contains_key(&parent) || self.nodes.contains_key(&id) {
            return false;
        }
        self.nodes
            .insert(id, NodeMeta::new(Some(parent), kind, name, termination));
        true
    }

    pub(super) fn remove_nodes(&mut self, ids: &[AgentId]) {
        for id in ids {
            self.nodes.remove(id);
        }
    }

    pub(super) fn root_children(&self) -> Vec<AgentId> {
        let Some(root) = self.root() else {
            return Vec::new();
        };
        self.nodes
            .iter()
            .filter_map(|(&id, meta)| (meta.parent == Some(root)).then_some(id))
            .collect()
    }

    pub(super) fn is_strict_descendant(&self, ancestor: AgentId, node: AgentId) -> bool {
        is_strict_descendant(ancestor, node, |id| {
            self.nodes.get(&id).and_then(|meta| meta.parent)
        })
    }

    pub(super) fn depth(&self, id: AgentId) -> Option<u16> {
        let mut current = id;
        let mut depth = 0_u16;
        let mut visited = BTreeSet::new();
        loop {
            if !visited.insert(current) {
                return None;
            }
            let meta = self.nodes.get(&current)?;
            let Some(parent) = meta.parent else {
                return Some(depth);
            };
            depth = depth.checked_add(1)?;
            current = parent;
        }
    }

    pub(super) fn subtree_ids(&self, root: AgentId) -> Vec<AgentId> {
        let mut out = Vec::new();
        let mut frontier = VecDeque::from([root]);
        let mut visited = BTreeSet::new();
        while let Some(current) = frontier.pop_front() {
            if !visited.insert(current) {
                continue;
            }
            out.push(current);
            for (&id, meta) in &self.nodes {
                if meta.parent == Some(current) {
                    frontier.push_back(id);
                }
            }
        }
        out
    }
}

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::instance) fn refresh_snapshots(&self) {
        let snapshot = AgentGraphSnapshot::new(self.state.get().graph.nodes().map(|(id, meta)| {
            AgentSnapshot {
                id,
                kind: meta.kind().clone(),
                name: meta.name().map(str::to_owned),
                parent: meta.parent(),
                depth: self
                    .state
                    .get()
                    .graph
                    .depth(id)
                    .expect("live graph topology is valid"),
                termination: meta.termination(),
                status: self
                    .state
                    .get()
                    .scheduler
                    .agent_status(id, self.inflight.contains(id)),
            }
        }));
        *self
            .snapshots
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = snapshot;
    }
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeMap;

    use super::{GraphState, NodeMeta};
    use crate::agent::{AgentId, AgentKind, TerminationPolicy};

    #[test]
    fn graph_state_owns_root_nodes_and_descendant_rules() {
        let root = AgentId(1);
        let child = AgentId(2);
        let grandchild = AgentId(3);
        let unrelated = AgentId(4);
        let mut graph = GraphState::default();
        assert!(graph.insert_root(root, AgentKind::from_static("test")));
        assert!(graph.insert_child(
            root,
            child,
            AgentKind::from_static("test"),
            None,
            TerminationPolicy::AutoOnIdle
        ));
        assert!(graph.insert_child(
            child,
            grandchild,
            AgentKind::from_static("test"),
            None,
            TerminationPolicy::AutoOnIdle
        ));

        assert!(graph.is_strict_descendant(root, child));
        assert!(graph.is_strict_descendant(root, grandchild));
        assert!(!graph.is_strict_descendant(root, root));
        assert!(!graph.is_strict_descendant(root, unrelated));
        assert_eq!(graph.depth(root), Some(0));
        assert_eq!(graph.depth(grandchild), Some(2));
        assert_eq!(graph.subtree_ids(child), vec![child, grandchild]);
    }

    #[test]
    fn subtree_walk_terminates_if_malformed_state_contains_a_cycle() {
        let first = AgentId(1);
        let second = AgentId(2);
        let nodes = BTreeMap::from([
            (
                first,
                NodeMeta::new(
                    Some(second),
                    AgentKind::from_static("test"),
                    None,
                    TerminationPolicy::AutoOnIdle,
                ),
            ),
            (
                second,
                NodeMeta::new(
                    Some(first),
                    AgentKind::from_static("test"),
                    None,
                    TerminationPolicy::AutoOnIdle,
                ),
            ),
        ]);
        let graph = GraphState::restored(nodes);

        assert_eq!(graph.subtree_ids(first), vec![first, second]);
    }
}
