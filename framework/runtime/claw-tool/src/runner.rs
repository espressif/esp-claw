use claw_permission::{Action, PermissionDecision};

use super::set::ToolSetHandle;
use super::tool::{ToolError, ToolInvocation, ToolInvokeError};

pub trait ToolGate {
    fn decide(&self, action: &Action) -> PermissionDecision;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApprovalNeeded {
    pub summary: String,
    pub signature: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ToolRunOutcome {
    Ran {
        content: String,
        ok: bool,
    },
    Blocked {
        content: String,
    },
    ApprovalNeeded {
        content: String,
        approval: ApprovalNeeded,
    },
}

pub struct ToolRunner<'a> {
    tools: &'a ToolSetHandle<'a>,
    gate: Option<&'a dyn ToolGate>,
}

impl<'a> ToolRunner<'a> {
    pub fn new(tools: &'a ToolSetHandle<'a>, gate: Option<&'a dyn ToolGate>) -> Self {
        Self { tools, gate }
    }

    pub async fn run<'call>(&self, call: &'call ToolInvocation<'call>) -> ToolRunOutcome {
        let action = match self.tools.classify(call) {
            Ok(action) => action,
            Err(error) => return render_before_permission(error),
        };

        match self.decide(&action) {
            PermissionDecision::Allow => self.invoke(call).await,
            PermissionDecision::Ask { reason } => ToolRunOutcome::ApprovalNeeded {
                content: reason.clone(),
                approval: ApprovalNeeded {
                    summary: reason,
                    signature: action.signature(),
                },
            },
            PermissionDecision::Deny { reason } => ToolRunOutcome::Blocked { content: reason },
        }
    }

    fn decide(&self, action: &Action) -> PermissionDecision {
        self.gate
            .map(|gate| gate.decide(action))
            .unwrap_or(PermissionDecision::Allow)
    }

    async fn invoke<'call>(&self, call: &'call ToolInvocation<'call>) -> ToolRunOutcome {
        match self.tools.invoke(call).await {
            Ok(output) => ToolRunOutcome::Ran {
                content: output.output,
                ok: output.ok,
            },
            Err(error) => render_after_execution(error),
        }
    }
}

fn render_before_permission(error: ToolInvokeError) -> ToolRunOutcome {
    match &error.error {
        ToolError::InvokeRejected(message) => ToolRunOutcome::Blocked {
            content: message.clone(),
        },
        _ => render_after_execution(error),
    }
}

fn render_after_execution(error: ToolInvokeError) -> ToolRunOutcome {
    ToolRunOutcome::Ran {
        content: error.to_string(),
        ok: false,
    }
}
