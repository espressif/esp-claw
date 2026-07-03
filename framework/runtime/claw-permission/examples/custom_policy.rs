//! Writing a custom [`PermissionPolicy`] and composing it with the built-ins.
//!
//! Here a rule hard-denies any write that targets a protected path, regardless
//! of risk. Chained ahead of `AskAtOrAbove`, its `Deny` short-circuits the chain
//! (most-restrictive-wins), so protected paths can never even be asked about.
//!
//! ```bash
//! cargo run --example custom_policy --target x86_64-unknown-linux-gnu
//! ```

use claw_permission::{
    Action, AskAtOrAbove, PermissionDecision, PermissionPolicy, PermissionRequest, PolicyChain,
    Resource, RiskClass,
};

/// Deny any action whose target path starts with one of the protected prefixes.
struct ProtectPaths {
    prefixes: Vec<String>,
}

impl ProtectPaths {
    fn new(prefixes: impl IntoIterator<Item = impl Into<String>>) -> Self {
        Self {
            prefixes: prefixes.into_iter().map(Into::into).collect(),
        }
    }
}

impl PermissionPolicy for ProtectPaths {
    fn evaluate(&self, request: &PermissionRequest<'_>) -> PermissionDecision {
        // Only paths are protected; other resources (or none) fall through to
        // the next policy in the chain via `Allow`.
        let Some(Resource::Path(path)) = request.action.resource() else {
            return PermissionDecision::Allow;
        };
        if self.prefixes.iter().any(|prefix| path.starts_with(prefix)) {
            PermissionDecision::Deny {
                reason: format!("'{path}' is a protected path"),
            }
        } else {
            PermissionDecision::Allow
        }
    }
}

fn main() {
    let policy = PolicyChain::new()
        .with(ProtectPaths::new(["/system", "/boot"]))
        .with(AskAtOrAbove::new(RiskClass::Moderate));

    let cases = [
        // Protected path: denied outright, never asked.
        Action::new("write_file", RiskClass::Moderate)
            .with_resource(Resource::Path("/system/config".into())),
        // Risky but unprotected: the chain asks.
        Action::new("write_file", RiskClass::Moderate)
            .with_resource(Resource::Path("/data/notes.txt".into())),
        // Safe and unprotected: allowed.
        Action::new("read_file", RiskClass::Safe)
            .with_resource(Resource::Path("/data/notes.txt".into())),
    ];

    for action in &cases {
        let request = PermissionRequest::new(action);
        let label = match action.resource() {
            Some(Resource::Path(path)) => path.clone(),
            _ => "(no resource)".to_string(),
        };
        println!(
            "{:<12} {:<18} -> {:?}",
            action.verb(),
            label,
            policy.evaluate(&request)
        );
    }
}
