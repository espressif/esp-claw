use std::sync::Arc;

use crate::agent::base_agent::AgentId;
use crate::agent::kind::AgentKind;
use crate::session::Message;

use super::types::{AgentGraphSnapshot, AgentSnapshot, GraphEffect, TerminationPolicy};

pub(crate) trait GraphHost: Send + Sync {
    fn next_id(&self) -> AgentId;

    fn emit(&self, requester: AgentId, effect: GraphEffect);

    fn snapshot(&self) -> AgentGraphSnapshot;
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
                goal: Message::text(goal),
                termination,
            },
        );
        child
    }

    pub(crate) fn delete_subagent(&self, target: AgentId) {
        self.host.emit(self.id, GraphEffect::Delete { target });
    }

    pub(crate) fn followup_subagent(&self, target: AgentId, message: String) {
        self.host.emit(
            self.id,
            GraphEffect::Followup {
                target,
                message: Message::text(message),
            },
        );
    }

    pub(crate) fn list_subagents(&self) -> Vec<AgentSnapshot> {
        self.host.snapshot().descendants_of(self.id)
    }

    pub(crate) fn get_subagent(&self, target: AgentId) -> Option<AgentSnapshot> {
        self.host.snapshot().descendant(self.id, target)
    }
}
