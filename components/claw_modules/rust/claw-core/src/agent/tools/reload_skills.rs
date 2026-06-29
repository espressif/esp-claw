//! `reload_skills()` — re-scan the skills directory so runtime additions show up.
//!
//! Discovery (`list_skills`) and loading (`load_skill`) read a cached catalog
//! snapshot, which is cheap but does not see skills added to disk after startup.
//! This tool pays the filesystem rescan **once, on demand** — call it after a
//! skill is installed/removed on disk, then `list_skills` / `load_skill` reflect
//! it. Keeping the scan out of the read paths means those stay I/O-free even when
//! called often.

use std::sync::Arc;

use claw_skill::SkillRegistry;
use claw_tool::{tool_metadata, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};

/// Re-scans the skill registry's roots and swaps in a fresh catalog.
pub(crate) struct ReloadSkillsTool {
    registry: Arc<dyn SkillRegistry>,
}

impl ReloadSkillsTool {
    /// Build the tool over a `registry` handle.
    pub(crate) fn new(registry: Arc<dyn SkillRegistry>) -> Self {
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
    use super::super::skill_tool_group;
    use super::super::test_support::{sink, skill_registry_with_fs, tool_named, write_skill};
    use super::*;
    use claw_interface::ClawFs;

    fn invoke(tool: &claw_tool::Tool, name: &str) -> ToolOutput {
        tool.invoke(&ToolInvocation {
            id: Some("t1"),
            name,
            arguments_json: "{}",
        })
        .unwrap()
    }

    #[test]
    fn reload_exposes_a_filesystem_addition_to_list_skills() {
        let (fs, registry) = skill_registry_with_fs(&[("alpha", "First skill")]);
        let group = skill_tool_group(sink(), registry);
        let reload = tool_named(&group, "reload_skills");
        let list = tool_named(&group, "list_skills");

        // Added after construction: invisible to the cached snapshot until reload.
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
        let group = skill_tool_group(sink(), registry);
        let reload = tool_named(&group, "reload_skills");

        // A malformed SKILL.md makes the rescan fail; the old catalog is kept.
        fs.write_atomic("skills/broken/SKILL.md", b"no front matter here")
            .unwrap();
        let output = invoke(&reload, "reload_skills");
        assert!(!output.ok);
        assert!(output.output.contains("Could not refresh skills"));
    }
}
