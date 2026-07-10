use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use claw_permission::{
    Action, Grant, GrantStore, PermissionDecision, PermissionPolicy, PermissionRequest,
};
use claw_tool::ToolGate;

use crate::agent::iteration_loop::InterruptionControl;

#[derive(Clone)]
pub(crate) struct AgentAbortHandle {
    flag: Arc<AtomicBool>,
}

impl AgentAbortHandle {
    pub(super) fn from_flag(flag: Arc<AtomicBool>) -> Self {
        Self { flag }
    }

    pub fn abort(&self) {
        self.flag.store(true, Ordering::Release);
    }
}

impl Default for AgentAbortHandle {
    fn default() -> Self {
        Self {
            flag: Arc::new(AtomicBool::new(false)),
        }
    }
}

pub(super) struct AgentInterruption {
    flag: Arc<AtomicBool>,
}

impl AgentInterruption {
    pub(super) fn new() -> Self {
        Self {
            flag: Arc::new(AtomicBool::new(false)),
        }
    }

    pub(super) fn handle(&self) -> AgentAbortHandle {
        AgentAbortHandle::from_flag(Arc::clone(&self.flag))
    }

    pub(super) fn clear(&self) {
        self.flag.store(false, Ordering::Release);
    }
}

impl InterruptionControl for AgentInterruption {
    fn interrupt_flag(&self) -> &Arc<AtomicBool> {
        &self.flag
    }
}

pub(super) struct PermissionGate<'a> {
    pub(super) policy: &'a dyn PermissionPolicy,
    pub(super) grants: &'a GrantStore,
}

impl ToolGate for PermissionGate<'_> {
    fn decide(&self, action: &Action) -> PermissionDecision {
        match self.grants.lookup(&action.signature()) {
            Some(Grant::Granted) => PermissionDecision::Allow,
            Some(Grant::Denied(reason)) => PermissionDecision::Deny {
                reason: reason.clone(),
            },
            None => self.policy.evaluate(&PermissionRequest::new(action)),
        }
    }
}
