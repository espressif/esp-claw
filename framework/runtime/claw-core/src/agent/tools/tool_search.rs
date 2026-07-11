//! `tool_search()` — list the hidden tool groups the model can reveal.

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, ToolDiscoveryHandle, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolSpec,
};
use serde_json::json;

/// Reads the owning [`ToolSet`](claw_tool::ToolSet)'s loadable catalog and
/// returns it — group ids, tool names, and short descriptions, never schemas.
pub(crate) struct ToolSearchTool {
    pub(super) discovery: ToolDiscoveryHandle,
}

impl ToolSpec for ToolSearchTool {
    tool_metadata!("tool_search");

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new(self.name(), RiskClass::Safe)
    }
}

impl SyncToolHandler for ToolSearchTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: json!({ "tool_groups": self.discovery.catalog() }).to_string(),
            ok: true,
        })
    }
}
