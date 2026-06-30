//! `end_conversation(final_message)` — the agent ends the task on its own terms.

use claw_tool::{tool_metadata, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};

use super::{push, string_argument, ControlSignal, ControlSink};

/// The self-control tool: pushes an [`EndConversation`](ControlSignal::EndConversation)
/// onto the agent's [`ControlSink`] for the next tick to act on.
pub(crate) struct EndConversationTool {
    sink: ControlSink,
}

impl EndConversationTool {
    /// Build the tool over the agent's control `sink`.
    pub(crate) fn new(sink: ControlSink) -> Self {
        Self { sink }
    }
}

impl ToolHandler for EndConversationTool {
    tool_metadata!("end_conversation");

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let final_message = string_argument(call.arguments_json, "final_message")?;
        push(&self.sink, ControlSignal::EndConversation { final_message });
        Ok(ToolOutput {
            output: "Conversation ended.".to_string(),
            ok: true,
        })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::super::{internal_tool_group, test_support::sink};
    use super::*;
    use claw_tool::ToolError;

    #[test]
    fn end_conversation_pushes_signal_with_message() {
        let sink = sink();
        let group = internal_tool_group(std::sync::Arc::clone(&sink));
        let tool = group
            .tools()
            .iter()
            .find(|tool| tool.name() == "end_conversation")
            .unwrap()
            .clone();

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "end_conversation",
                arguments_json: r#"{"final_message":"all done"}"#,
            })
            .unwrap();
        assert!(output.ok);

        let signal = sink.lock().unwrap().pop_front().unwrap();
        assert_eq!(
            signal,
            ControlSignal::EndConversation {
                final_message: "all done".to_string()
            }
        );
    }

    #[test]
    fn malformed_arguments_error_not_swallowed() {
        let group = internal_tool_group(sink());
        let tool = group
            .tools()
            .iter()
            .find(|tool| tool.name() == "end_conversation")
            .unwrap()
            .clone();

        let error = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "end_conversation",
                arguments_json: "{not json",
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvalidArgumentsJson(_)));
        assert!(error.retries.is_none());
    }
}
