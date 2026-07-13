//! `tool_load(group_id)` — reveal one hidden tool group for the next turn.

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, ToolDiscoveryHandle, ToolError, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolSpec,
};
use serde_json::json;

use super::optional_string_argument;

/// Queues a group's tools to be enabled on the owning
/// [`ToolSet`](claw_tool::ToolSet)'s next tick, via the discovery bridge.
pub(crate) struct ToolLoadTool {
    pub(super) discovery: ToolDiscoveryHandle,
}

impl ToolSpec for ToolLoadTool {
    tool_metadata!("tool_load");

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new(self.name(), RiskClass::Safe)
    }
}

impl SyncToolHandler for ToolLoadTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let group_id = optional_string_argument(call.arguments_json(), "group_id")?
            .map(|group_id| group_id.trim().to_owned())
            .filter(|group_id| !group_id.is_empty())
            .ok_or_else(|| {
                ToolError::InvalidArguments("tool_load 'group_id' is required".into())
            })?;

        let loaded = self.discovery.request_load(group_id.clone());
        Ok(ToolOutput {
            output: json!({
                "group_id": group_id,
                "loaded": loaded,
                "available_next_turn": loaded,
            })
            .to_string(),
            ok: loaded,
        })
    }
}
