use claw_permission::{
    Action, AllowAll, AskAtOrAbove, Grant, GrantStore, PermissionDecision, PermissionPolicy,
    PermissionRequest, PolicyChain, RiskClass,
};

#[test]
fn grant_then_lookup_returns_granted() {
    let mut store = GrantStore::new();
    store.grant("write_file:path:/a");
    assert_eq!(store.lookup("write_file:path:/a"), Some(&Grant::Granted));
    assert_eq!(store.lookup("write_file:path:/b"), None);
}

#[test]
fn deny_records_reason_and_forget_clears() {
    let mut store = GrantStore::new();
    store.deny("rm:path:/a", "too risky");
    assert_eq!(
        store.lookup("rm:path:/a"),
        Some(&Grant::Denied("too risky".into()))
    );
    store.forget("rm:path:/a");
    assert_eq!(store.lookup("rm:path:/a"), None);
}

#[test]
fn allow_all_allows() {
    let action = Action::new("anything", RiskClass::High);
    assert_eq!(
        AllowAll.evaluate(&PermissionRequest::new(&action)),
        PermissionDecision::Allow
    );
}

#[test]
fn ask_at_or_above_thresholds_on_risk() {
    let policy = AskAtOrAbove::new(RiskClass::Moderate);
    let safe = Action::new("read", RiskClass::Safe);
    let risky = Action::new("write", RiskClass::Moderate);

    assert_eq!(
        policy.evaluate(&PermissionRequest::new(&safe)),
        PermissionDecision::Allow
    );
    assert!(matches!(
        policy.evaluate(&PermissionRequest::new(&risky)),
        PermissionDecision::Ask { .. }
    ));
}

#[test]
fn chain_is_most_restrictive_wins() {
    let action = Action::new("write", RiskClass::Moderate);

    let ask_chain = PolicyChain::new()
        .with(AskAtOrAbove::new(RiskClass::Moderate))
        .with(AllowAll);
    assert!(matches!(
        ask_chain.evaluate(&PermissionRequest::new(&action)),
        PermissionDecision::Ask { .. }
    ));

    let deny_chain = PolicyChain::new()
        .with(AskAtOrAbove::new(RiskClass::Moderate))
        .with(DenyAll);
    assert!(matches!(
        deny_chain.evaluate(&PermissionRequest::new(&action)),
        PermissionDecision::Deny { .. }
    ));
}

#[test]
fn empty_chain_allows() {
    let action = Action::new("x", RiskClass::High);
    assert_eq!(
        PolicyChain::new().evaluate(&PermissionRequest::new(&action)),
        PermissionDecision::Allow
    );
}

struct DenyAll;

impl PermissionPolicy for DenyAll {
    fn evaluate(&self, _request: &PermissionRequest<'_>) -> PermissionDecision {
        PermissionDecision::Deny {
            reason: "nope".into(),
        }
    }
}
