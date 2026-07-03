//! The full `Ask` round-trip: a policy asks for approval, a human decides, the
//! decision is recorded in a [`GrantStore`], and the retried call resolves from
//! the store instead of asking again (which also prevents an ask/retry loop).
//!
//! ```bash
//! cargo run --example approval_flow --target x86_64-unknown-linux-gnu
//! ```

use claw_permission::{
    Action, AskAtOrAbove, Grant, GrantStore, PermissionDecision, PermissionPolicy,
    PermissionRequest, Resource, RiskClass,
};

/// Resolve a call the way the runtime does: a recorded decision wins over the
/// policy; otherwise fall back to evaluating the policy.
fn resolve(
    policy: &dyn PermissionPolicy,
    grants: &GrantStore,
    request: &PermissionRequest<'_>,
) -> PermissionDecision {
    match grants.lookup(&request.action.signature()) {
        Some(Grant::Granted) => PermissionDecision::Allow,
        Some(Grant::Denied(reason)) => PermissionDecision::Deny {
            reason: reason.clone(),
        },
        None => policy.evaluate(request),
    }
}

fn main() {
    let policy = AskAtOrAbove::new(RiskClass::Moderate);
    let mut grants = GrantStore::new();

    let action = Action::new("write_file", RiskClass::Moderate)
        .with_resource(Resource::Path("/data/report.txt".into()));
    let request = PermissionRequest::new(&action);
    println!("signature: {}\n", action.signature());

    // 1. First attempt — nothing recorded, so the policy asks.
    let first = resolve(&policy, &grants, &request);
    println!("attempt 1 (no grant yet): {first:?}");
    assert!(matches!(first, PermissionDecision::Ask { .. }));

    // 2. A human approves; the runtime records it against the signature.
    grants.grant(action.signature());
    println!("\n-> human approved; recorded grant\n");

    // 3. The retried call now resolves directly — no second prompt, no loop.
    let second = resolve(&policy, &grants, &request);
    println!("attempt 2 (after grant): {second:?}");
    assert_eq!(second, PermissionDecision::Allow);

    // A denial works the same way: the reason flows back to the model.
    let risky = Action::new("delete_file", RiskClass::High)
        .with_resource(Resource::Path("/data/report.txt".into()));
    let risky_request = PermissionRequest::new(&risky);
    grants.deny(risky.signature(), "destructive, never auto-run");
    let denied = resolve(&policy, &grants, &risky_request);
    println!("\ndelete_file (after denial): {denied:?}");
    assert!(matches!(denied, PermissionDecision::Deny { .. }));
}
