use std::collections::VecDeque;

use claw_tool::ToolRunOutcome;
use serde::{Deserialize, Serialize};

use crate::agent::iteration_loop::{AppendedMessages, ToolsOutcome};

/// A tool round withheld from the transcript until each `Ask` call has a real
/// result. Calls that already finished are represented only by their messages.
#[derive(Deserialize, Serialize)]
pub(super) struct PendingToolRound {
    appended: AppendedMessages,
    calls: VecDeque<PendingToolCall>,
    blocked_tools: Vec<String>,
}

#[derive(Deserialize, Serialize)]
pub(super) struct PendingToolCall {
    pub(super) name: String,
    pub(super) tool_call_id: String,
    pub(super) arguments_json: String,
    pub(super) summary: String,
    pub(super) signature: String,
}

impl PendingToolRound {
    pub(super) fn from_tools(tools: ToolsOutcome) -> Option<Self> {
        let ToolsOutcome { appended, runs } = tools;
        let calls = runs
            .into_iter()
            .filter_map(|run| {
                run.into_approval().map(|(name, approval)| PendingToolCall {
                    name,
                    tool_call_id: approval.tool_call_id,
                    arguments_json: approval.arguments_json,
                    summary: approval.summary,
                    signature: approval.signature,
                })
            })
            .collect::<VecDeque<_>>();
        if calls.is_empty() {
            return None;
        }
        Some(Self {
            appended,
            calls,
            blocked_tools: Vec::new(),
        })
    }

    pub(super) fn next(&self) -> Option<&PendingToolCall> {
        self.calls.front()
    }

    pub(super) fn pop_next(
        mut self,
    ) -> Result<(PendingToolCall, PendingToolRound), PendingToolRoundError> {
        let call = self
            .calls
            .pop_front()
            .ok_or(PendingToolRoundError::NoPendingApproval)?;
        Ok((call, self))
    }

    pub(super) fn resolve(
        mut self,
        call: PendingToolCall,
        outcome: ToolRunOutcome,
    ) -> Result<PendingToolRound, PendingToolRoundError> {
        let (content, ok, blocked) = match outcome {
            ToolRunOutcome::Ran { content, ok } => (content, ok, false),
            ToolRunOutcome::Blocked { content } => (content, false, true),
            ToolRunOutcome::ApprovalNeeded { .. } => {
                return Err(PendingToolRoundError::ApprovalStillRequired)
            }
        };
        if !self
            .appended
            .replace_tool_result(&call.tool_call_id, content, ok)
        {
            return Err(PendingToolRoundError::MissingToolResult {
                tool_call_id: call.tool_call_id,
            });
        }
        if blocked {
            self.blocked_tools.push(call.name);
        }
        Ok(self)
    }

    pub(super) fn into_completed(self) -> (AppendedMessages, Vec<String>) {
        (self.appended, self.blocked_tools)
    }

    #[cfg(test)]
    pub(super) fn pending_for_test(signature: &str) -> Self {
        Self::from_tools(ToolsOutcome::pending_for_test(signature))
            .expect("test tool outcome contains one pending approval")
    }
}

#[derive(Debug, thiserror::Error)]
pub(super) enum PendingToolRoundError {
    #[error("pending tool round has no approval to resolve")]
    NoPendingApproval,
    #[error("resolved tool call still requires approval")]
    ApprovalStillRequired,
    #[error("pending tool round has no result slot for {tool_call_id}")]
    MissingToolResult { tool_call_id: String },
}
