//! Soft-tools phase gating: the full schema is always advertised to the model
//! (so the cached `tools` prefix never moves), while the [`ToolSet`] restricts
//! which tools may actually *run* this phase — and reports when the model keeps
//! calling a blocked tool past the retry budget.
//!
//! ```bash
//! cargo run --example soft_tools --target x86_64-unknown-linux-gnu
//! ```

use claw_tool::{
    AllowedTools, BlockPolicy, Tool, ToolBlockVerdict, ToolHandler, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolSet,
};

/// A demo tool carrying soft-tools `usage` prose for the `tool_context` block.
struct DemoTool {
    name: String,
    schema: String,
    usage: Option<String>,
}

impl DemoTool {
    fn new(name: &str, usage: Option<&str>) -> Self {
        Self {
            schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
            name: name.to_string(),
            usage: usage.map(str::to_string),
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

    fn usage(&self) -> Option<&str> {
        self.usage.as_deref()
    }

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: format!("{} ran", self.name),
            ok: true,
        })
    }
}

fn main() -> anyhow::Result<()> {
    let mut set = ToolSet::new([
        Tool::new(DemoTool::new(
            "read_file",
            Some("Read a file from the workspace."),
        )),
        Tool::new(DemoTool::new(
            "write_file",
            Some("Write a file. Mutates the workspace."),
        )),
    ])?;

    // The static usage block — stable, name-ordered, belongs in the cached prefix.
    println!("== tool_context (static usage block) ==");
    println!("{}\n", set.tool_context().unwrap_or("(none)"));

    // Enter a read-only phase. The schema still advertises *both* tools, but only
    // `read_file` may execute now.
    set.set_active_tools(AllowedTools::new(["read_file"]));
    println!("== extra_tool_context (dynamic phase note, request tail) ==");
    println!("{}\n", set.extra_tool_context().unwrap_or("(ungated)"));
    println!("read_file  allowed? {}", set.is_allowed("read_file"));
    println!("write_file allowed? {}\n", set.is_allowed("write_file"));

    // Retry-then-fail: the model ignores the restriction and keeps calling the
    // blocked tool. After the budget (here: 1 nudge) the round is `Exhausted`.
    // The streak counter is a separate `BlockPolicy` the agent owns (not the
    // tool catalog), so the cached wire surfaces above stay immutable.
    let mut block_policy = BlockPolicy::new(1);
    println!("== retry-then-fail (block_retries = 1) ==");
    for round in 1..=3 {
        let verdict = block_policy.record_round(&["write_file"]);
        println!("round {round}: model called blocked write_file -> {verdict:?}");
        if let ToolBlockVerdict::Exhausted { name } = verdict {
            println!("  budget exhausted on \"{name}\"; the agent should end the task");
            break;
        }
    }

    Ok(())
}
