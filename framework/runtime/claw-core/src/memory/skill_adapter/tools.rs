//! Skill tools owned by the skill context adapter.

use std::sync::{Arc, Mutex};

use claw_skill::{SkillError, SkillId, SkillSet};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};
use serde_json::Value;

use super::lock_skill_set;

pub(super) fn skill_tools(skills: Arc<Mutex<SkillSet>>) -> Vec<Tool> {
    vec![
        Tool::from_sync(ListSkillTool::new(Arc::clone(&skills))),
        Tool::from_sync(ActivateSkillTool::new(Arc::clone(&skills))),
        Tool::from_sync(ReloadSkillsTool::new(skills)),
    ]
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

/// Serves the available-skills JSON catalog resolved from the agent's SkillSet.
struct ListSkillTool {
    skills: Arc<Mutex<SkillSet>>,
}

impl ListSkillTool {
    fn new(skills: Arc<Mutex<SkillSet>>) -> Self {
        Self { skills }
    }
}

impl ToolSpec for ListSkillTool {
    tool_metadata!("list_skill");
}

impl SyncToolHandler for ListSkillTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let mut skills = lock_skill_set(&self.skills);
        let output = match skills.list_skill() {
            Ok(output) => output.to_owned(),
            Err(error) => {
                return Ok(ToolOutput {
                    output: format!("Could not list skills: {error}"),
                    ok: false,
                });
            }
        };
        Ok(ToolOutput { output, ok: true })
    }
}

/// Activates one skill and returns its processed document as the tool result.
struct ActivateSkillTool {
    skills: Arc<Mutex<SkillSet>>,
}

impl ActivateSkillTool {
    fn new(skills: Arc<Mutex<SkillSet>>) -> Self {
        Self { skills }
    }
}

impl ToolSpec for ActivateSkillTool {
    tool_metadata!("activate_skill");
}

impl SyncToolHandler for ActivateSkillTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let skill_id = string_argument(call.arguments_json(), "skill_id")?;
        let skill_id = skill_id.trim();
        if skill_id.is_empty() {
            return Err(ToolError::InvokeRejected(
                "`skill_id` is required: pass the id of a skill from list_skill.".to_string(),
            )
            .into());
        }

        let mut skills = lock_skill_set(&self.skills);
        match skills.activate_skill(&SkillId::new(skill_id)) {
            Ok(document) => Ok(ToolOutput {
                output: document.into_content(),
                ok: true,
            }),
            Err(SkillError::NotFound(_)) => Err(ToolError::InvokeRejected(format!(
                "unknown skill \"{skill_id}\"; call list_skill to see what is available."
            ))
            .into()),
            Err(error) => Ok(ToolOutput {
                output: format!("Could not activate skill \"{skill_id}\": {error}"),
                ok: false,
            }),
        }
    }
}

/// Re-scans the skill registry's roots and swaps in a fresh catalog.
struct ReloadSkillsTool {
    skills: Arc<Mutex<SkillSet>>,
}

impl ReloadSkillsTool {
    fn new(skills: Arc<Mutex<SkillSet>>) -> Self {
        Self { skills }
    }
}

impl ToolSpec for ReloadSkillsTool {
    tool_metadata!("reload_skills");
}

impl SyncToolHandler for ReloadSkillsTool {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let skills = lock_skill_set(&self.skills);
        if let Err(error) = skills.reload() {
            return Ok(ToolOutput {
                output: format!("Could not refresh skills from disk: {error}"),
                ok: false,
            });
        }
        Ok(ToolOutput {
            output: "Skills refreshed. Use list_skill to inspect the catalog.".to_string(),
            ok: true,
        })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use claw_interface::ClawFs;
    use claw_tool::{RawToolInvocation, ToolRegistry};

    use super::super::test_support::{
        skill_registry, skill_registry_with_fs, skill_tools_for_test, tool_named, write_skill,
    };
    use super::*;

    fn call<'a>(name: &'a str, arguments_json: &'a str) -> ToolInvocation<'a> {
        ToolInvocation::try_from(RawToolInvocation {
            id: Some("t1"),
            name,
            arguments_json,
        })
        .unwrap()
    }

    fn invoke_result(
        tool: &claw_tool::Tool,
        name: &str,
        arguments_json: &str,
    ) -> Result<ToolOutput, ToolInvokeError> {
        let registry = std::sync::Arc::new(ToolRegistry::new());
        let mut tools = registry.tool_set();
        tools.add_tool(tool.clone()).unwrap();
        let handle = tools.begin().unwrap();
        let call = call(name, arguments_json);
        futures_lite::future::block_on(handle.invoke(&call))
    }

    fn invoke(tool: &claw_tool::Tool, name: &str) -> ToolOutput {
        invoke_result(tool, name, "{}").unwrap()
    }

    #[test]
    fn lists_every_available_skill_as_json() {
        let registry = skill_registry(&[("alpha", "First skill"), ("beta", "Second skill")]);
        let tools = skill_tools_for_test(registry.skill_set());
        let tool = tool_named(&tools, "list_skill");

        let output = invoke(&tool, "list_skill");
        assert!(output.ok);
        assert!(output.output.contains(r#""id":"alpha""#));
        assert!(output.output.contains(r#""description":"Second skill""#));
    }

    #[test]
    fn reports_empty_catalog_as_json_array() {
        let tools = skill_tools_for_test(SkillSet::empty());
        let tool = tool_named(&tools, "list_skill");

        let output = invoke(&tool, "list_skill");
        assert!(output.ok);
        assert_eq!(output.output, "[]");
    }

    #[test]
    fn activates_a_known_skill_as_tool_output() {
        let registry = skill_registry(&[("alpha", "First skill")]);
        let tools = skill_tools_for_test(registry.skill_set());
        let tool = tool_named(&tools, "activate_skill");

        let output = invoke_result(&tool, "activate_skill", r#"{"skill_id":"alpha"}"#).unwrap();
        assert!(output.ok);
        assert!(output.output.starts_with(r#"<skill_content name="alpha">"#));
        assert!(output.output.contains("Body for alpha."));
        assert!(output.output.ends_with("</skill_content>"));
    }

    #[test]
    fn unknown_skill_is_rejected() {
        let registry = skill_registry(&[("alpha", "First skill")]);
        let tools = skill_tools_for_test(registry.skill_set());
        let tool = tool_named(&tools, "activate_skill");

        let error =
            invoke_result(&tool, "activate_skill", r#"{"skill_id":"missing"}"#).unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
    }

    #[test]
    fn blank_activate_skill_is_rejected() {
        let registry = skill_registry(&[("alpha", "First skill")]);
        let tools = skill_tools_for_test(registry.skill_set());
        let tool = tool_named(&tools, "activate_skill");

        let error = invoke_result(&tool, "activate_skill", r#"{"skill_id":"   "}"#).unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
    }

    #[test]
    fn reload_exposes_a_filesystem_addition_to_list_skill() {
        let (fs, registry) = skill_registry_with_fs(&[("alpha", "First skill")]);
        let tools = skill_tools_for_test(registry.skill_set());
        let reload = tool_named(&tools, "reload_skills");
        let list = tool_named(&tools, "list_skill");

        write_skill(&fs, "gamma", "Late skill");
        assert!(!invoke(&list, "list_skill").output.contains("gamma"));

        let refreshed = invoke(&reload, "reload_skills");
        assert!(refreshed.ok);
        assert!(invoke(&list, "list_skill").output.contains("gamma"));
    }

    #[test]
    fn reload_reports_failure_without_panicking() {
        let (fs, registry) = skill_registry_with_fs(&[("alpha", "First skill")]);
        let tools = skill_tools_for_test(registry.skill_set());
        let reload = tool_named(&tools, "reload_skills");

        fs.write_atomic("skills/broken/SKILL.md", b"no front matter here")
            .unwrap();
        let output = invoke(&reload, "reload_skills");
        assert!(!output.ok);
        assert!(output.output.contains("Could not refresh skills"));
    }
}
