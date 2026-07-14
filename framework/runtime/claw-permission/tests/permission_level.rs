use claw_permission::{
    Action, PermissionDecision, PermissionLevel, PermissionPolicy, PermissionRequest, RiskClass,
};

#[test]
fn permission_levels_control_side_effects_without_blocking_safe_actions() {
    let safe = Action::new("read", RiskClass::Safe);
    let mutation = Action::new("write", RiskClass::Low);

    for level in [
        PermissionLevel::Deny,
        PermissionLevel::Ask,
        PermissionLevel::AllowAll,
    ] {
        assert_eq!(
            level.evaluate(&PermissionRequest::new(&safe)),
            PermissionDecision::Allow
        );
    }

    assert!(matches!(
        PermissionLevel::Deny.evaluate(&PermissionRequest::new(&mutation)),
        PermissionDecision::Deny { .. }
    ));
    assert!(matches!(
        PermissionLevel::Ask.evaluate(&PermissionRequest::new(&mutation)),
        PermissionDecision::Ask { .. }
    ));
    assert_eq!(
        PermissionLevel::AllowAll.evaluate(&PermissionRequest::new(&mutation)),
        PermissionDecision::Allow
    );
}

#[test]
fn allow_all_is_the_compatibility_default() {
    assert_eq!(PermissionLevel::default(), PermissionLevel::AllowAll);
}
