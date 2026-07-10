use std::sync::Arc;

use crate::agent::base_agent::AgentId;
use crate::agent::kind::AgentKind;

use super::types::{AgentSnapshot, GraphEffect, TerminationPolicy};

pub(crate) trait GraphHost: Send + Sync {
    fn next_id(&self) -> AgentId;

    fn emit(&self, requester: AgentId, effect: GraphEffect);

    fn snapshot(&self) -> Vec<AgentSnapshot>;
}

pub(crate) struct AgentContext {
    id: AgentId,
    host: Arc<dyn GraphHost>,
}

impl AgentContext {
    pub(crate) fn new(id: AgentId, host: Arc<dyn GraphHost>) -> Self {
        Self { id, host }
    }

    pub(crate) fn spawn(
        &self,
        kind: AgentKind,
        name: Option<String>,
        goal: String,
        termination: TerminationPolicy,
    ) -> AgentId {
        let child = self.host.next_id();
        self.host.emit(
            self.id,
            GraphEffect::Spawn {
                id: child,
                kind,
                name,
                goal,
                termination,
            },
        );
        child
    }

    pub(crate) fn delete_subagent(&self, target: AgentId) {
        self.host.emit(self.id, GraphEffect::Delete { target });
    }

    pub(crate) fn list_subagents(&self) -> Vec<AgentSnapshot> {
        let all = self.host.snapshot();
        let mut descendants: Vec<AgentSnapshot> = all
            .iter()
            .filter(|snapshot| is_strict_descendant(&all, self.id, snapshot.id))
            .cloned()
            .collect();
        descendants.sort_by_key(|snapshot| (snapshot.depth, snapshot.id.0));
        descendants
    }

    pub(crate) fn get_subagent(&self, target: AgentId) -> Option<AgentSnapshot> {
        let all = self.host.snapshot();
        is_strict_descendant(&all, self.id, target)
            .then(|| all.into_iter().find(|snapshot| snapshot.id == target))
            .flatten()
    }
}

fn snapshot_parent(all: &[AgentSnapshot], id: AgentId) -> Option<AgentId> {
    all.iter()
        .find(|snapshot| snapshot.id == id)
        .and_then(|snapshot| snapshot.parent)
}

fn is_strict_descendant(all: &[AgentSnapshot], ancestor: AgentId, node: AgentId) -> bool {
    let mut current = snapshot_parent(all, node);
    while let Some(parent) = current {
        if parent == ancestor {
            return true;
        }
        current = snapshot_parent(all, parent);
    }
    false
}
