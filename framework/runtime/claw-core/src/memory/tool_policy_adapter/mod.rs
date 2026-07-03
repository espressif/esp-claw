//! Tool policy context adapter.
//!
//! The source is the agent's [`ToolSet`]; this adapter projects its cached prompt
//! surfaces into `claw-context`.

use claw_context::{Block, BlockKind, ContextSink};

use super::traits::{ContextAdapter, ContextAdapterInput};

const ADAPTER_ID: &str = "tool_policy";

pub(crate) struct ToolPolicyContextAdapter;

impl ToolPolicyContextAdapter {
    pub(crate) fn new() -> Self {
        Self
    }
}

impl ContextAdapter for ToolPolicyContextAdapter {
    fn id(&self) -> &str {
        ADAPTER_ID
    }

    fn contribute(&mut self, input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        output.block(Block::new(
            BlockKind::ToolPolicy,
            input.tools.tool_context().unwrap_or_default(),
        ));
        output.reminder(BlockKind::ToolReminder, input.tools.extra_tool_context());
    }
}
