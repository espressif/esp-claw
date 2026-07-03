//! Composing built-in policies into a chain and classifying actions of varying
//! risk. The chain is most-restrictive-wins: a single `Deny` short-circuits, an
//! `Ask` beats `Allow`, an empty chain allows everything.
//!
//! ```bash
//! cargo run --example policy_chain --target x86_64-unknown-linux-gnu
//! ```

use claw_permission::{
    Action, AllowAll, AskAtOrAbove, PermissionDecision, PermissionPolicy, PermissionRequest,
    PolicyChain, Resource, RiskClass,
};

/// Render a decision compactly for printing.
fn show(decision: &PermissionDecision) -> String {
    match decision {
        PermissionDecision::Allow => "Allow".to_string(),
        PermissionDecision::Ask { reason } => format!("Ask({reason})"),
        PermissionDecision::Deny { reason } => format!("Deny({reason})"),
    }
}

fn main() {
    // "Allow unless risky": ask at or above Moderate, allow everything else.
    let policy = PolicyChain::new()
        .with(AskAtOrAbove::new(RiskClass::Moderate))
        .with(AllowAll);

    let actions = [
        Action::new("read_file", RiskClass::Safe)
            .with_resource(Resource::Path("/data/notes.txt".into())),
        Action::new("write_file", RiskClass::Moderate)
            .with_resource(Resource::Path("/data/notes.txt".into())),
        Action::new("delete_file", RiskClass::High)
            .with_resource(Resource::Path("/data/notes.txt".into())),
    ];

    println!("== AskAtOrAbove(Moderate) over a permissive base ==");
    for action in &actions {
        let request = PermissionRequest::new(action);
        println!(
            "{:<12} risk={:?}  ->  {}",
            action.verb(),
            action.risk(),
            show(&policy.evaluate(&request))
        );
    }
}
