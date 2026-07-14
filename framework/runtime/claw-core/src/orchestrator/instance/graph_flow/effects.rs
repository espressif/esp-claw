use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::agent::{AgentId, AgentKind, AgentPlacement, GraphEffect, TerminationPolicy};
use crate::session::Message;

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

    fn apply_followup(&mut self, requester: AgentId, target: AgentId, message: Message) {
        if !self
            .state
            .get()
            .graph
            .is_strict_descendant(requester, target)
        {
            tracing::warn!(
                name: "followup_ignored",
                target_agent = %target,
                reason = "not_descendant",
            );
            return;
        }
        if !self.state.get().graph.contains(target) {
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
        if !self
            .state
            .get()
            .graph
            .is_strict_descendant(requester, target)
        {
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
        let victims = self.state.get().graph.subtree_ids(root);
        tracing::info!(
            name: "subtree_deleted",
            root_agent = %root,
            count = victims.len() as u64,
        );
        for victim in &victims {
            self.registry.remove(*victim);
        }
        let state = self.state.get_mut();
        state.graph.remove_nodes(&victims);
        state.scheduler.remove_agents(&victims);
    }

    pub(in crate::orchestrator::instance) fn delete_spawned_subagents(&mut self) {
        let children = self.state.get().graph.root_children();
        for child in children {
            self.delete_subtree(child);
        }
    }

    fn materialize_spawn(
        &mut self,
        parent: AgentId,
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        goal: Message,
        termination: TerminationPolicy,
    ) {
        if !self.state.get().graph.contains(parent) {
            tracing::warn!(
                name: "spawn_dropped",
                parent_agent = %parent,
                kind = %kind.as_str(),
                reason = "missing_parent",
            );
            return;
        }
        match self.build_agent(id, &kind, goal, AgentPlacement::Sub(id), Vec::new()) {
            Ok(()) => {
                let inserted = self.state.get_mut().graph.insert_child(
                    parent,
                    id,
                    kind.clone(),
                    name,
                    termination,
                );
                if !inserted {
                    self.registry.remove(id);
                    tracing::warn!(
                        name: "spawn_dropped",
                        parent_agent = %parent,
                        kind = %kind.as_str(),
                        reason = "missing_parent",
                    );
                    return;
                }
                tracing::info!(
                    name: "spawn_materialized",
                    parent_agent = %parent,
                    child_agent = %id,
                    kind = %kind.as_str(),
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
}
