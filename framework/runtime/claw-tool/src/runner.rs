use claw_permission::{Action, PermissionDecision};

use super::set::ToolSetHandle;
use super::tool::{ToolError, ToolInvocation};

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
            Err(error) => {
                return match &error.error {
                    ToolError::InvokeRejected(message) => ToolRunOutcome::Blocked {
                        content: message.clone(),
                    },
                    _ => ToolRunOutcome::Ran {
                        content: error.to_string(),
                        ok: false,
                    },
                };
            }
        };

        let decision = match self.gate {
            Some(gate) => gate.decide(&action),
            None => PermissionDecision::Allow,
        };
        match decision {
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

    async fn invoke<'call>(&self, call: &'call ToolInvocation<'call>) -> ToolRunOutcome {
        match self.tools.invoke(call).await {
            Ok(output) => ToolRunOutcome::Ran {
                content: output.output,
                ok: output.ok,
            },
            Err(error) => ToolRunOutcome::Ran {
                content: error.to_string(),
                ok: false,
            },
        }
    }
}
