use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use claw_permission::{Action, PermissionDecision, PermissionPolicy, PermissionRequest};
use claw_tool::ToolGate;

use crate::agent::iteration_loop::InterruptionControl;

use super::ApprovalDecision;

const PENDING_ACTION_CHANGED: &str = "the pending tool action changed before execution";

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
}

impl ToolGate for PermissionGate<'_> {
    fn decide(&self, action: &Action) -> PermissionDecision {
        self.policy.evaluate(&PermissionRequest::new(action))
    }
}

pub(super) struct ResolvedPermissionGate<'a> {
    pub(super) policy: &'a dyn PermissionPolicy,
    pub(super) expected_signature: &'a str,
    pub(super) decision: &'a ApprovalDecision,
}

impl ToolGate for ResolvedPermissionGate<'_> {
    fn decide(&self, action: &Action) -> PermissionDecision {
        if action.signature() != self.expected_signature {
            return PermissionDecision::Deny {
                reason: PENDING_ACTION_CHANGED.to_owned(),
            };
        }
        match self.decision {
            ApprovalDecision::Rejected(reason) => PermissionDecision::Deny {
                reason: reason.clone(),
            },
            ApprovalDecision::Approved => {
                match self.policy.evaluate(&PermissionRequest::new(action)) {
                    PermissionDecision::Ask { .. } | PermissionDecision::Allow => {
                        PermissionDecision::Allow
                    }
                    decision @ PermissionDecision::Deny { .. } => decision,
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use claw_permission::{Action, PermissionDecision, PermissionLevel, RiskClass};
    use claw_tool::ToolGate;

    use super::{PermissionGate, ResolvedPermissionGate};
    use crate::agent::ApprovalDecision;

    #[test]
    fn permission_gate_projects_the_current_policy() {
        let action = Action::new("write", RiskClass::High);
        let gate = PermissionGate {
            policy: &PermissionLevel::Ask,
        };
        assert!(matches!(
            gate.decide(&action),
            PermissionDecision::Ask { .. }
        ));

        let gate = PermissionGate {
            policy: &PermissionLevel::AllowAll,
        };
        assert_eq!(gate.decide(&action), PermissionDecision::Allow);
    }

    #[test]
    fn resolved_gate_applies_one_matching_decision_without_persisting_a_grant() {
        let action = Action::new("write", RiskClass::High);
        let signature = action.signature();
        let approved = ResolvedPermissionGate {
            policy: &PermissionLevel::Ask,
            expected_signature: &signature,
            decision: &ApprovalDecision::Approved,
        };
        assert_eq!(approved.decide(&action), PermissionDecision::Allow);
        assert!(matches!(
            approved.decide(&Action::new("delete", RiskClass::High)),
            PermissionDecision::Deny { .. }
        ));

        let denied_by_current_policy = ResolvedPermissionGate {
            policy: &PermissionLevel::Deny,
            expected_signature: &signature,
            decision: &ApprovalDecision::Approved,
        };
        assert!(matches!(
            denied_by_current_policy.decide(&action),
            PermissionDecision::Deny { .. }
        ));

        let rejected = ResolvedPermissionGate {
            policy: &PermissionLevel::AllowAll,
            expected_signature: &signature,
            decision: &ApprovalDecision::Rejected("blocked".into()),
        };
        assert_eq!(
            rejected.decide(&action),
            PermissionDecision::Deny {
                reason: "blocked".into()
            }
        );
    }
}
