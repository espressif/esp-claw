//! `load_skill(skill)` — pull one skill's guidance into context for later turns.
//!
//! The id is validated against the [`registry`](claw_skill::SkillRegistry)'s
//! cached catalog synchronously (no filesystem I/O), so an unknown skill is
//! rejected here (and the model can retry) rather than failing silently later. A
//! valid call pushes a [`LoadSkill`](ControlSignal::LoadSkill) onto the agent's
//! [`ControlSink`] for the next tick to apply. For a skill just added to disk,
//! call `reload_skills` first so it is in the catalog.

use std::sync::Arc;

use claw_skill::{SkillId, SkillRegistry};
use claw_tool::{
    tool_metadata, ToolError, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput,
};

use super::{push, string_argument, ControlSignal, ControlSink};

/// Loads a skill into context via the agent's control sink, after checking the
/// id exists in `registry`.
pub(crate) struct LoadSkillTool {
    sink: ControlSink,
    registry: Arc<dyn SkillRegistry>,
}

impl LoadSkillTool {
    /// Build the tool over the agent's control `sink` and a `registry` handle.
    pub(crate) fn new(sink: ControlSink, registry: Arc<dyn SkillRegistry>) -> Self {
        Self { sink, registry }
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
        if self.registry.metadata(&id).is_none() {
            return Err(ToolError::invoke_rejected(format!(
                "unknown skill \"{skill}\"; call list_skills to see what is available."
            ))
            .into());
        }
        push(&self.sink, ControlSignal::LoadSkill { id });
        Ok(ToolOutput {
            output: format!("Skill \"{skill}\" loaded; its guidance is now in context."),
            ok: true,
        })
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::super::skill_tool_group;
    use super::super::test_support::{sink, skill_registry, tool_named};
    use super::*;

    #[test]
    fn loads_a_known_skill_and_pushes_signal() {
        let sink = sink();
        let registry = skill_registry(&[("alpha", "First skill")]);
        let group = skill_tool_group(std::sync::Arc::clone(&sink), registry);
        let tool = tool_named(&group, "load_skill");

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "load_skill",
                arguments_json: r#"{"skill":"alpha"}"#,
            })
            .unwrap();
        assert!(output.ok);

        let signal = sink.lock().unwrap().pop_front().unwrap();
        assert_eq!(
            signal,
            ControlSignal::LoadSkill {
                id: SkillId::new("alpha")
            }
        );
    }

    #[test]
    fn unknown_skill_is_rejected_without_a_signal() {
        let sink = sink();
        let registry = skill_registry(&[("alpha", "First skill")]);
        let group = skill_tool_group(std::sync::Arc::clone(&sink), registry);
        let tool = tool_named(&group, "load_skill");

        let error = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "load_skill",
                arguments_json: r#"{"skill":"missing"}"#,
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
        assert!(sink.lock().unwrap().is_empty());
    }

    #[test]
    fn blank_skill_is_rejected() {
        let registry = skill_registry(&[("alpha", "First skill")]);
        let group = skill_tool_group(sink(), registry);
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
}
