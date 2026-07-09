//! `end_conversation(final_message)` — the agent ends the task on its own terms.

use claw_tool::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use super::{optional_string_argument, ControlSignal, ControlSink};

/// The self-control tool: pushes an [`EndConversation`](ControlSignal::EndConversation)
/// onto the agent's [`ControlSink`] for the next tick to act on.
pub(crate) struct EndConversationTool {
    pub(super) sink: ControlSink,
}

impl ToolSpec for EndConversationTool {
    tool_metadata!("end_conversation");
}

impl SyncToolHandler for EndConversationTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let Some(final_message) = optional_string_argument(call.arguments_json(), "final_message")?
        else {
            return Err(ToolError::InvalidArguments(
                "end_conversation 'final_message' is required".into(),
            )
            .into());
        };
        let final_message = final_message.trim();
        if final_message.is_empty() {
            return Err(ToolError::InvalidArguments(
                "end_conversation 'final_message' is required".into(),
            )
            .into());
        }
        self.sink
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .push_back(ControlSignal::EndConversation {
                final_message: final_message.to_string(),
            });
        Ok(ToolOutput {
            output: "Conversation ended.".to_string(),
            ok: true,
        })
    }
}
