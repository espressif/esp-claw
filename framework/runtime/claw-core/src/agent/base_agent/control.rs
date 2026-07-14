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

    pub(crate) fn abort(&self) {
        self.flag.store(true, Ordering::Release);
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
        if let Some(Grant::Denied(reason)) = self.grants.lookup(&action.signature()) {
            return PermissionDecision::Deny {
                reason: reason.clone(),
            };
        }
        let decision = self.policy.evaluate(&PermissionRequest::new(action));
        match decision {
            ask @ PermissionDecision::Ask { .. } => match self.grants.lookup(&action.signature()) {
                Some(Grant::Granted) => PermissionDecision::Allow,
                Some(Grant::Denied(_)) | None => ask,
            },
            decision @ (PermissionDecision::Allow | PermissionDecision::Deny { .. }) => decision,
        }
    }
}

#[cfg(test)]
mod tests {
    use claw_permission::{Action, GrantStore, PermissionDecision, PermissionLevel, RiskClass};
    use claw_tool::ToolGate;

    use super::PermissionGate;

    #[test]
    fn permission_gate_preserves_denials_and_uses_approvals_only_for_ask() {
        let action = Action::new("write", RiskClass::High);
        let mut grants = GrantStore::new();
        let gate = PermissionGate {
            policy: &PermissionLevel::Ask,
            grants: &grants,
        };
        assert!(matches!(
            gate.decide(&action),
            PermissionDecision::Ask { .. }
        ));

        grants.deny(action.signature(), "blocked");
        let gate = PermissionGate {
            policy: &PermissionLevel::Ask,
            grants: &grants,
        };
        assert_eq!(
            gate.decide(&action),
            PermissionDecision::Deny {
                reason: "blocked".into()
            }
        );
        let gate = PermissionGate {
            policy: &PermissionLevel::AllowAll,
            grants: &grants,
        };
        assert_eq!(
            gate.decide(&action),
            PermissionDecision::Deny {
                reason: "blocked".into()
            }
        );

        grants.grant(action.signature());
        let gate = PermissionGate {
            policy: &PermissionLevel::Deny,
            grants: &grants,
        };
        assert!(matches!(
            gate.decide(&action),
            PermissionDecision::Deny { .. }
        ));
    }
}
