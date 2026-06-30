//! Executing tool calls through the [`ToolRunner`]: soft-hide gating, then a
//! permission [`ToolGate`], then dispatch. The runner returns a neutral
//! [`CallOutcome`] (ran / blocked / denied / approval-needed) the caller renders
//! into a tool message.
//!
//! This uses a hand-written gate to keep the example self-contained; the real
//! agent installs [`claw_tool::PermissionGate`], which answers from a policy plus
//! a store of recorded human decisions.
//!
//! ```bash
//! cargo run --example run_with_gate --target x86_64-unknown-linux-gnu
//! ```

use claw_permission::{Action, PermissionDecision, RiskClass};
use claw_tool::{
    Tool, ToolGate, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolRunner, ToolSet,
};

/// A demo tool that classifies a "write" verb as risky so the permission path is
/// exercised; everything else is safe.
struct DemoTool {
    name: String,
    schema: String,
}

impl DemoTool {
    fn new(name: &str) -> Self {
        Self {
            schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
            name: name.to_string(),
        }
    }
}

impl ToolHandler for DemoTool {
    fn name(&self) -> &str {
        &self.name
    }

    fn schema(&self) -> &str {
        &self.schema
    }

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        let risk = if self.name.starts_with("write") {
            RiskClass::High
        } else {
            RiskClass::Safe
        };
        Action::new(self.name.clone(), risk)
    }

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: format!("{} ran", self.name),
            ok: true,
        })
    }
}

/// A toy policy: ask before any high-risk action, allow the rest.
struct DemoGate;

impl ToolGate for DemoGate {
    fn decide(&self, action: &Action) -> PermissionDecision {
        if action.risk() >= RiskClass::High {
            PermissionDecision::Ask {
                reason: format!("Confirm \"{}\"?", action.verb()),
            }
        } else {
            PermissionDecision::Allow
        }
    }
}

fn call(name: &str) -> ToolInvocation<'_> {
    ToolInvocation {
        id: Some("t1"),
        name,
        arguments_json: "{}",
    }
}

fn main() -> anyhow::Result<()> {
    let tools = ToolSet::new([
        Tool::new(DemoTool::new("read_file")),
        Tool::new(DemoTool::new("write_file")),
    ])?;
    let gate = DemoGate;
    let runner = ToolRunner::new(&tools, Some(&gate));

    // Safe tool: allowed, runs.
    let outcome = runner.run_one(&call("read_file"));
    println!(
        "read_file  -> ok={} content={:?}",
        outcome.ok, outcome.content
    );

    // High-risk tool: the gate asks for approval; the tool does NOT run.
    let outcome = runner.run_one(&call("write_file"));
    match outcome.approval {
        Some(approval) => println!(
            "write_file -> approval needed: summary={:?} signature={:?}",
            approval.summary, approval.signature
        ),
        None => println!(
            "write_file -> ok={} content={:?}",
            outcome.ok, outcome.content
        ),
    }

    Ok(())
}
