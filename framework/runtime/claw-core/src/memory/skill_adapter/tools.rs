//! Skill-management tools owned by the skill context adapter.

use std::sync::Arc;

use claw_skill::{SkillError, SkillId, SkillRegistry};
use claw_tool::{
    tool_metadata, Tool, ToolError, ToolGroup, ToolHandler, ToolInvocation, ToolInvokeError,
    ToolOutput,
};
use serde_json::Value;

use super::SkillAdapterState;

const SKILL_TOOL_GROUP: &str = "skills";

pub(super) fn skill_tool_group(state: Arc<SkillAdapterState>) -> ToolGroup {
    let registry = state.registry();
    ToolGroup::new(
        SKILL_TOOL_GROUP,
        [
            Tool::new(ListSkillsTool::new(Arc::clone(&registry))),
            Tool::new(LoadSkillTool::new(Arc::clone(&state))),
            Tool::new(UnloadSkillTool::new(state)),
            Tool::new(ReloadSkillsTool::new(registry)),
        ],
    )
}

fn string_argument(arguments_json: &str, key: &str) -> Result<String, ToolError> {
    if arguments_json.trim().is_empty() {
        return Ok(String::new());
    }
    let value: Value = serde_json::from_str(arguments_json).map_err(|error| {
        ToolError::InvalidArgumentsJson(format!("invalid tool arguments JSON: {error}"))
    })?;
    Ok(value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string())
}

/// Serves the available-skills menu resolved from the agent's skill registry.
struct ListSkillsTool {
    registry: Arc<dyn SkillRegistry>,
}

impl ListSkillsTool {
    fn new(registry: Arc<dyn SkillRegistry>) -> Self {
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

/// Loads a skill into this adapter's active-skill set.
struct LoadSkillTool {
    state: Arc<SkillAdapterState>,
}

impl LoadSkillTool {
    fn new(state: Arc<SkillAdapterState>) -> Self {
        Self { state }
    }
}

impl ToolHandler for LoadSkillTool {
    tool_metadata!("load_skill");

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let skill = string_argument(call.arguments_json, "skill")?;
        let skill = skill.trim();
        if skill.is_empty() {
            return Err(ToolError::invoke_rejected(
                "`skill` is required: pass the id of a skill from list_skills.",
            )
            .into());
        }
        let id = SkillId::new(skill);
        match self.state.load(id) {
            Ok(()) => Ok(ToolOutput {
                output: format!("Skill \"{skill}\" loaded; its guidance is now in context."),
                ok: true,
            }),
            Err(SkillError::NotFound(_)) => Err(ToolError::invoke_rejected(format!(
                "unknown skill \"{skill}\"; call list_skills to see what is available."
            ))
            .into()),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not load skill \"{skill}\": {error}"),
                ok: false,
            }),
        }
    }
}

/// Unloads a skill from this adapter's active-skill set.
struct UnloadSkillTool {
    state: Arc<SkillAdapterState>,
}

impl UnloadSkillTool {
    fn new(state: Arc<SkillAdapterState>) -> Self {
        Self { state }
    }
}

impl ToolHandler for UnloadSkillTool {
    tool_metadata!("unload_skill");

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let skill = string_argument(call.arguments_json, "skill")?;
        let skill = skill.trim();
        if skill.is_empty() {
            return Err(ToolError::invoke_rejected(
                "`skill` is required: pass the id of a loaded skill.",
            )
            .into());
        }
        self.state.unload(&SkillId::new(skill));
        Ok(ToolOutput {
            output: format!("Skill \"{skill}\" unloaded."),
            ok: true,
        })
    }
}

/// Re-scans the skill registry's roots and swaps in a fresh catalog.
struct ReloadSkillsTool {
    registry: Arc<dyn SkillRegistry>,
}

impl ReloadSkillsTool {
    fn new(registry: Arc<dyn SkillRegistry>) -> Self {
        Self { registry }
    }
}

impl ToolHandler for ReloadSkillsTool {
    tool_metadata!("reload_skills");

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        if let Err(error) = self.registry.reload() {
            return Ok(ToolOutput {
                output: format!("Could not refresh skills from disk: {error}"),
                ok: false,
            });
        }
        let count = self.registry.catalog().entries().len();
        Ok(ToolOutput {
            output: format!("Skills refreshed; {count} available. Use list_skills to see them."),
            ok: true,
        })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use claw_interface::ClawFs;
    use claw_skill::{SkillId, SkillSet};

    use super::super::test_support::{
        skill_registry, skill_registry_with_fs, skill_tool_group_for_test, tool_named, write_skill,
    };
    use super::*;

    fn invoke(tool: &claw_tool::Tool, name: &str) -> ToolOutput {
        tool.invoke(&ToolInvocation {
            id: Some("t1"),
            name,
            arguments_json: "{}",
        })
        .unwrap()
    }

    #[test]
    fn lists_every_available_skill() {
        let registry = skill_registry(&[("alpha", "First skill"), ("beta", "Second skill")]);
        let group = skill_tool_group_for_test(SkillSet::new(registry));
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
        let group = skill_tool_group_for_test(SkillSet::new(skill_registry(&[])));
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

    #[test]
    fn loads_a_known_skill_into_the_adapter_state() {
        let state = Arc::new(SkillAdapterState::new(SkillSet::new(skill_registry(&[(
            "alpha",
            "First skill",
        )]))));
        let group = skill_tool_group(Arc::clone(&state));
        let tool = tool_named(&group, "load_skill");

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "load_skill",
                arguments_json: r#"{"skill":"alpha"}"#,
            })
            .unwrap();
        assert!(output.ok);

        let loaded = state.lock().context().unwrap().to_string();
        assert!(loaded.contains("Body for alpha."));
    }

    #[test]
    fn unknown_skill_is_rejected_without_loading() {
        let state = Arc::new(SkillAdapterState::new(SkillSet::new(skill_registry(&[(
            "alpha",
            "First skill",
        )]))));
        let group = skill_tool_group(Arc::clone(&state));
        let tool = tool_named(&group, "load_skill");

        let error = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "load_skill",
                arguments_json: r#"{"skill":"missing"}"#,
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
        assert!(state.lock().is_empty());
    }

    #[test]
    fn blank_load_skill_is_rejected() {
        let group =
            skill_tool_group_for_test(SkillSet::new(skill_registry(&[("alpha", "First skill")])));
        let tool = tool_named(&group, "load_skill");

        let error = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "load_skill",
                arguments_json: r#"{"skill":"   "}"#,
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
    }

    #[test]
    fn unload_drops_loaded_skill_from_the_adapter_state() {
        let registry = skill_registry(&[("alpha", "First skill")]);
        let mut skills = SkillSet::new(registry);
        skills.load("test", SkillId::new("alpha")).unwrap();
        let state = Arc::new(SkillAdapterState::new(skills));
        let group = skill_tool_group(Arc::clone(&state));
        let tool = tool_named(&group, "unload_skill");

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "unload_skill",
                arguments_json: r#"{"skill":"alpha"}"#,
            })
            .unwrap();
        assert!(output.ok);
        assert!(state.lock().is_empty());
    }

    #[test]
    fn blank_unload_skill_is_rejected() {
        let group = skill_tool_group_for_test(SkillSet::new(skill_registry(&[])));
        let tool = tool_named(&group, "unload_skill");

        let error = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "unload_skill",
                arguments_json: r#"{"skill":""}"#,
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
    }

    #[test]
    fn reload_exposes_a_filesystem_addition_to_list_skills() {
        let (fs, registry) = skill_registry_with_fs(&[("alpha", "First skill")]);
        let group = skill_tool_group_for_test(SkillSet::new(registry));
        let reload = tool_named(&group, "reload_skills");
        let list = tool_named(&group, "list_skills");

        write_skill(&fs, "gamma", "Late skill");
        assert!(!invoke(&list, "list_skills").output.contains("gamma"));

        let refreshed = invoke(&reload, "reload_skills");
        assert!(refreshed.ok);
        assert!(invoke(&list, "list_skills")
            .output
            .contains("gamma: Late skill"));
    }

    #[test]
    fn reload_reports_failure_without_panicking() {
        let (fs, registry) = skill_registry_with_fs(&[("alpha", "First skill")]);
        let group = skill_tool_group_for_test(SkillSet::new(registry));
        let reload = tool_named(&group, "reload_skills");

        fs.write_atomic("skills/broken/SKILL.md", b"no front matter here")
            .unwrap();
        let output = invoke(&reload, "reload_skills");
        assert!(!output.ok);
        assert!(output.output.contains("Could not refresh skills"));
    }
}
