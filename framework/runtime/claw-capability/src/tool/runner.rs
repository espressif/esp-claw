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
            PermissionDecision::Allow => self.invoke_with_retry(call).await,
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

    async fn invoke_with_retry<'call>(&self, call: &'call ToolInvocation<'call>) -> ToolRunOutcome {
        let mut retries = None;
        loop {
            match self.tools.invoke(call).await {
                Ok(output) => {
                    return ToolRunOutcome::Ran {
                        content: output.output,
                        ok: output.ok,
                    };
                }
                Err(error) => {
                    let remaining = retries.get_or_insert_with(|| error.retries.extra_attempts());
                    if *remaining == 0 {
                        return render_after_execution(error);
                    }
                    *remaining = (*remaining).saturating_sub(1);
                }
            }
        }
    }
}

fn render_before_permission(error: ToolInvokeError) -> ToolRunOutcome {
    let ToolInvokeError { error, retries } = error;
    match error {
        ToolError::InvokeRejected(message) => ToolRunOutcome::Blocked { content: message },
        error => render_after_execution(ToolInvokeError { error, retries }),
    }
}

fn render_after_execution(error: ToolInvokeError) -> ToolRunOutcome {
    ToolRunOutcome::Ran {
        content: render_tool_error(error.error),
        ok: false,
    }
}

fn render_tool_error(error: ToolError) -> String {
    match error {
        ToolError::NotFound(name) => {
            let mut text = String::from("tool not found: ");
            text.push_str(&name);
            text
        }
        ToolError::InvalidArgumentsJson(message) => {
            let mut text = String::from("invalid arguments json: ");
            text.push_str(&message);
            text
        }
        ToolError::InvalidArguments(message) => {
            let mut text = String::from("invalid arguments: ");
            text.push_str(&message);
            text
        }
        ToolError::InvokeRejected(message) => {
            let mut text = String::from("tool invocation rejected: ");
            text.push_str(&message);
            text
        }
    }
}
