//! The permission gate: the [`ToolGate`] implementation the runner consults.
//!
//! A [`PermissionGate`] bridges the runner's [`ToolGate`] seam to a
//! [`PermissionPolicy`]: it carries the policy, the acting agent's identity, and
//! the [`GrantStore`] of recorded human decisions. It lives here (beside the
//! `ToolGate` trait) rather than in the agent layer so all tool *may-it-run*
//! gating is owned by `claw-tool`; the agent only owns the *lifecycle* of raising
//! an approval and feeding the answer back via [`record_decision`](PermissionGate::record_decision).

use std::sync::Arc;

use claw_permission::{
    Action, Grant, GrantStore, PermissionDecision, PermissionPolicy, PermissionRequest,
};

use crate::runner::ToolGate;

/// The agent's permission gate: a policy, the acting agent's identity, and the
/// grant store of human decisions, implementing [`ToolGate`] for the tool runner.
///
/// [`decide`](ToolGate::decide) is read-only — it answers from a recorded
/// [`Grant`] first (so a previously approved/denied action resolves without
/// asking again, which also prevents an ask/retry loop), then falls back to the
/// policy. Recording a decision happens separately, after a human answers, via
/// [`record_decision`](Self::record_decision).
pub struct PermissionGate {
    policy: Arc<dyn PermissionPolicy>,
    agent_id: u64,
    agent_kind: String,
    grants: GrantStore,
}

impl PermissionGate {
    /// Build a gate over `policy` for the agent identified by `agent_id` /
    /// `agent_kind`, starting with no recorded decisions.
    pub fn new(
        policy: Arc<dyn PermissionPolicy>,
        agent_id: u64,
        agent_kind: impl Into<String>,
    ) -> Self {
        Self {
            policy,
            agent_id,
            agent_kind: agent_kind.into(),
            grants: GrantStore::new(),
        }
    }

    /// Record a human decision against `signatures` (the actions that were asked
    /// about), so the matching retried calls resolve directly without asking
    /// again. `grant` is applied to every signature.
    pub fn record_decision(&mut self, signatures: &[String], grant: &Grant) {
        for signature in signatures {
            match grant {
                Grant::Granted => self.grants.grant(signature.clone()),
                Grant::Denied(reason) => self.grants.deny(signature.clone(), reason.clone()),
            }
        }
    }
}

impl ToolGate for PermissionGate {
    fn decide(&self, action: &Action) -> PermissionDecision {
        // A recorded decision wins over the policy: it both honors the human and
        // breaks the ask -> retry -> ask loop.
        match self.grants.lookup(&action.signature()) {
            Some(Grant::Granted) => return PermissionDecision::Allow,
            Some(Grant::Denied(reason)) => {
                return PermissionDecision::Deny {
                    reason: reason.clone(),
                }
            }
            None => {}
        }
        self.policy.evaluate(&PermissionRequest::new(
            self.agent_id,
            &self.agent_kind,
            action,
        ))
    }
}
