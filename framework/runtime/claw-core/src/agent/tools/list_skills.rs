//! `list_skills()` — the menu of skills the agent may load into context.
//!
//! A cheap read of the skill [`registry`](claw_skill::SkillRegistry)'s cached
//! catalog (no filesystem I/O), rendered as `id: description` rows so the model
//! can pick a skill id for `load_skill` instead of guessing — mirroring how
//! `list_spawnable_agents` precedes `spawn_subagent`. To pick up skills added to
//! disk at runtime, call `reload_skills` first.

use std::sync::Arc;

use claw_skill::SkillRegistry;
use claw_tool::{tool_metadata, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};

/// Serves the available-skills menu resolved from the agent's skill registry.
pub(crate) struct ListSkillsTool {
    registry: Arc<dyn SkillRegistry>,
}

impl ListSkillsTool {
    /// Build the tool over a `registry` handle.
    pub(crate) fn new(registry: Arc<dyn SkillRegistry>) -> Self {
        Self { registry }
    }
}

impl ToolHandler for ListSkillsTool {
    tool_metadata!("list_skills");

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let snapshot = self.registry.catalog();
        let entries = snapshot.entries();
        let output = if entries.is_empty() {
            "No skills are available to load.".to_string()
        } else {
            let mut out = String::from("Available skills:\n");
            for metadata in entries {
                out.push_str("- ");
                out.push_str(metadata.id().as_str());
                out.push_str(": ");
                out.push_str(metadata.description());
                out.push('\n');
            }
            out
        };
        Ok(ToolOutput { output, ok: true })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::super::skill_tool_group;
    use super::super::test_support::{sink, skill_registry, tool_named};
    use super::*;

    #[test]
    fn lists_every_available_skill() {
        let registry = skill_registry(&[("alpha", "First skill"), ("beta", "Second skill")]);
        let group = skill_tool_group(sink(), registry);
        let tool = tool_named(&group, "list_skills");

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "list_skills",
                arguments_json: "{}",
            })
            .unwrap();
        assert!(output.ok);
        assert!(output.output.contains("alpha: First skill"));
        assert!(output.output.contains("beta: Second skill"));
    }

    #[test]
    fn reports_when_no_skills_are_available() {
        let registry = skill_registry(&[]);
        let group = skill_tool_group(sink(), registry);
        let tool = tool_named(&group, "list_skills");

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "list_skills",
                arguments_json: "{}",
            })
            .unwrap();
        assert!(output.ok);
        assert!(output.output.contains("No skills are available"));
    }
}
