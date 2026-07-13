use std::collections::VecDeque;
use std::sync::Arc;

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentId, AgentKind, AgentPlacement, GraphEffect, TerminationPolicy};

use super::super::model::NodeMeta;
use super::super::OrchestratorInstance;

impl<Filesystem, Http, Timer> OrchestratorInstance<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::instance) fn apply_effects(&mut self) {
        let effects: Vec<(AgentId, GraphEffect)> = self
            .effects
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .drain(..)
            .collect();
        for (requester, effect) in effects {
            match effect {
                GraphEffect::Spawn {
                    id,
                    kind,
                    name,
                    goal,
                    termination,
                } => self.materialize_spawn(requester, id, kind, name, goal, termination),
                GraphEffect::Delete { target } => self.apply_delete(requester, target),
                GraphEffect::Followup { target, message } => {
                    self.apply_followup(requester, target, message)
                }
            }
        }
    }

    fn apply_followup(&mut self, requester: AgentId, target: AgentId, message: String) {
        if !self.is_descendant(requester, target) {
            tracing::warn!(
                name: "followup_ignored",
                target_agent = %target,
                reason = "not_descendant",
            );
            return;
        }
        if !self.state.get().meta.contains_key(&target) {
            tracing::warn!(
                name: "followup_ignored",
                target_agent = %target,
                reason = "missing_target",
            );
            return;
        }
        if self.inflight.abort_if_present(target) {
            tracing::info!(
                name: "followup_deferred",
                target_agent = %target,
                reason = "inflight_abort",
            );
            self.effects
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .push_back((requester, GraphEffect::Followup { target, message }));
            return;
        }
        if let Err(error) = self.deliver_followup(target, message) {
            tracing::warn!(
                name: "followup_ignored",
                target_agent = %target,
                reason = "delivery_failed",
                error = %error,
            );
        }
    }

    fn apply_delete(&mut self, requester: AgentId, target: AgentId) {
        if !self.is_descendant(requester, target) {
            tracing::warn!(
                name: "delete_ignored",
                target_agent = %target,
                reason = "not_descendant",
            );
            return;
        }
        self.delete_subtree(target);
    }

    pub(super) fn delete_subtree(&mut self, root: AgentId) {
        let victims = self.subtree_ids(root);
        tracing::info!(
            name: "subtree_deleted",
            root_agent = %root,
            count = victims.len() as u64,
        );
        for victim in &victims {
            self.state.get_mut().registry.remove(*victim);
            self.state.get_mut().meta.remove(victim);
            self.state.get_mut().parked_approvals.remove(victim);
        }
        self.state
            .get_mut()
            .ready
            .retain(|queued| !victims.contains(queued));
        self.state
            .get_mut()
            .approval_queue
            .retain(|queued| !victims.contains(queued));
        self.state
            .get_mut()
            .subagent_result_mailbox
            .retain(|result| !victims.contains(&result.parent) && !victims.contains(&result.child));
    }

    pub(in crate::orchestrator::instance) fn delete_spawned_subagents(&mut self) {
        let Some(root) = self.state.get().root else {
            return;
        };
        let children: Vec<AgentId> = self
            .state
            .get()
            .meta
            .iter()
            .filter_map(|(&id, meta)| (meta.parent == Some(root)).then_some(id))
            .collect();
        for child in children {
            self.delete_subtree(child);
        }
    }

    fn is_descendant(&self, ancestor: AgentId, node: AgentId) -> bool {
        let mut current = self
            .state
            .get()
            .meta
            .get(&node)
            .and_then(|meta| meta.parent);
        while let Some(parent) = current {
            if parent == ancestor {
                return true;
            }
            current = self
                .state
                .get()
                .meta
                .get(&parent)
                .and_then(|meta| meta.parent);
        }
        false
    }

    fn materialize_spawn(
        &mut self,
        parent: AgentId,
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        goal: String,
        termination: TerminationPolicy,
    ) {
        let Some(parent_meta) = self.state.get().meta.get(&parent) else {
            tracing::warn!(
                name: "spawn_dropped",
                parent_agent = %parent,
                kind = %kind.as_str(),
                reason = "missing_parent",
            );
            return;
        };
        let depth = parent_meta.depth.saturating_add(1);
        match self.build_agent(id, &kind, goal, AgentPlacement::Sub(id), Arc::from([])) {
            Ok(()) => {
                tracing::info!(
                    name: "spawn_materialized",
                    parent_agent = %parent,
                    child_agent = %id,
                    kind = %kind.as_str(),
                );
                self.state.get_mut().meta.insert(
                    id,
                    NodeMeta {
                        parent: Some(parent),
                        depth,
                        kind,
                        name,
                        termination,
                    },
                );
                self.enqueue(id);
            }
            Err(error) => {
                tracing::error!(
                    name: "spawn_dropped",
                    parent_agent = %parent,
                    kind = %kind.as_str(),
                    reason = "build_failed",
                    error = %error,
                );
            }
        }
    }

    fn subtree_ids(&self, root: AgentId) -> Vec<AgentId> {
        let mut out = vec![root];
        let mut frontier = VecDeque::from([root]);
        while let Some(current) = frontier.pop_front() {
            for (&id, meta) in &self.state.get().meta {
                if meta.parent == Some(current) {
                    out.push(id);
                    frontier.push_back(id);
                }
            }
        }
        out
    }
}
