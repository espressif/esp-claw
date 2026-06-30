//! `ToolRegistry` as a pool: register tools once, then carve per-agent
//! [`ToolSet`]s out of it by name.
//!
//! Run on the host:
//!
//! ```bash
//! cargo run --example registry --target x86_64-unknown-linux-gnu
//! ```

use claw_tool::{Tool, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolRegistry};

/// A trivial demo tool. A real tool bakes its `name`/`schema`/`usage` from
/// `resources/tools/<name>/` via the `tool_metadata!` macro; here we build them
/// inline so the example is self-contained (and to show that runtime-registered
/// tools may carry *owned*, dynamic names).
struct DemoTool {
    name: String,
    schema: String,
}

impl DemoTool {
    fn new(name: &str) -> Self {
        Self {
            schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
            name: name.to_string(),
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

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: format!("{} echoed: {}", self.name, call.arguments_json),
            ok: true,
        })
    }
}

fn main() -> anyhow::Result<()> {
    // The registry owns every tool the system knows about.
    let mut registry = ToolRegistry::new();
    registry.register(Tool::new(DemoTool::new("read_file")));
    registry.register(Tool::new(DemoTool::new("write_file")));
    registry.register(Tool::new(DemoTool::new("web_search")));
    println!("registered {} tools", registry.len());

    // An agent's tool set is a *selection* from the pool. The combined schema
    // array (sent to the LLM) is precomputed once at assembly.
    let set = registry.select(&["read_file", "web_search"])?;
    println!("\n== selected set schema (sent to the LLM) ==");
    println!("{}", set.schemas_json().unwrap_or("[]"));

    // Selecting an unknown name is a hard error, not a silent skip.
    match registry.select(&["read_file", "missing"]) {
        Err(error) => println!("\nselect rejected unknown tool: {error}"),
        Ok(_) => println!("\nunexpected: a missing tool was selected"),
    }

    Ok(())
}
