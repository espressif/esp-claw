//! `unload_skill(skill)` — drop one loaded skill's guidance from context.
//!
//! Pushes an [`UnloadSkill`](ControlSignal::UnloadSkill) onto the agent's
//! [`ControlSink`] for the next tick. Unloading a skill that was not loaded is a
//! no-op (the agent's [`SkillSet`](claw_skill::SkillSet) treats it as such), so
//! no registry check is needed here — only that an id was supplied.

use claw_skill::SkillId;
use claw_tool::{
    tool_metadata, ToolError, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput,
};

use super::{push, string_argument, ControlSignal, ControlSink};

/// Unloads a skill from context via the agent's control sink.
pub(crate) struct UnloadSkillTool {
    sink: ControlSink,
}

impl UnloadSkillTool {
    /// Build the tool over the agent's control `sink`.
    pub(crate) fn new(sink: ControlSink) -> Self {
        Self { sink }
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
        push(
            &self.sink,
            ControlSignal::UnloadSkill {
                id: SkillId::new(skill),
            },
        );
        Ok(ToolOutput {
            output: format!("Skill \"{skill}\" unloaded."),
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
    fn unload_pushes_signal_with_id() {
        let sink = sink();
        let group = skill_tool_group(std::sync::Arc::clone(&sink), skill_registry(&[]));
        let tool = tool_named(&group, "unload_skill");

        let output = tool
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "unload_skill",
                arguments_json: r#"{"skill":"alpha"}"#,
            })
            .unwrap();
        assert!(output.ok);

        let signal = sink.lock().unwrap().pop_front().unwrap();
        assert_eq!(
            signal,
            ControlSignal::UnloadSkill {
                id: SkillId::new("alpha")
            }
        );
    }

    #[test]
    fn blank_skill_is_rejected() {
        let group = skill_tool_group(sink(), skill_registry(&[]));
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
}
