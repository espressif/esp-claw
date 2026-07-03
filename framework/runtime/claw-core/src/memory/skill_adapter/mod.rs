//! Skill context adapter.
//!
//! This adapter owns the runtime [`SkillSet`] source for an agent. It projects
//! the loaded skill bodies into `BlockKind::ActiveSkills` and exposes the
//! skill-management tools that mutate the same source.

mod tools;

use std::sync::{Arc, Mutex, MutexGuard};

use claw_context::{Block, BlockKind, ContextSink};
use claw_skill::{SkillError, SkillId, SkillRegistry, SkillSet};
use claw_tool::ToolGroup;

use super::traits::{ContextAdapter, ContextAdapterInput};

const ADAPTER_ID: &str = "skills";
const MODEL_SKILL_GROUP: &str = "model";

pub(crate) struct SkillContextAdapter {
    state: Arc<SkillAdapterState>,
}

impl SkillContextAdapter {
    pub(crate) fn new(skills: SkillSet) -> Self {
        Self {
            state: Arc::new(SkillAdapterState::new(skills)),
        }
    }
}

impl ContextAdapter for SkillContextAdapter {
    fn id(&self) -> &str {
        ADAPTER_ID
    }

    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        let mut skills = self.state.lock();
        match skills.context() {
            Ok(rendered) => {
                output.block(Block::new(BlockKind::ActiveSkills, rendered));
            }
            Err(error) => {
                tracing::warn!(%error, "rebuilding active-skills context failed");
            }
        }
    }

    fn tools(&self) -> Vec<ToolGroup> {
        vec![tools::skill_tool_group(Arc::clone(&self.state))]
    }
}

struct SkillAdapterState {
    skills: Mutex<SkillSet>,
}

impl SkillAdapterState {
    fn new(skills: SkillSet) -> Self {
        Self {
            skills: Mutex::new(skills),
        }
    }

    fn lock(&self) -> MutexGuard<'_, SkillSet> {
        self.skills
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn registry(&self) -> Arc<dyn SkillRegistry> {
        self.lock().registry()
    }

    fn load(&self, id: SkillId) -> Result<(), SkillError> {
        self.lock().load(MODEL_SKILL_GROUP, id)
    }

    fn unload(&self, id: &SkillId) {
        self.lock().unload(id);
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
pub(crate) mod test_support {
    use std::sync::Arc;

    use claw_interface::{ClawFs, MemFs};
    use claw_skill::{FsSkillRegistry, SkillRegistry, SkillSet};
    use claw_tool::{Tool, ToolGroup};

    use super::{tools::skill_tool_group, SkillAdapterState};

    pub(crate) fn skill_tool_group_for_test(skills: SkillSet) -> ToolGroup {
        skill_tool_group(Arc::new(SkillAdapterState::new(skills)))
    }

    /// Write a minimal `SKILL.md` for `id` under the `skills` root of `fs`, so
    /// both catalog scans and document reads succeed.
    pub(crate) fn write_skill(fs: &MemFs, id: &str, description: &str) {
        let document = format!(
            "---\n{{\"name\":\"{id}\",\"description\":\"{description}\"}}\n---\n# {id}\n\nBody for {id}.\n"
        );
        fs.write_atomic(&format!("skills/{id}/SKILL.md"), document.as_bytes())
            .unwrap();
    }

    /// An in-memory skill registry seeded with `(id, description)` rows.
    pub(crate) fn skill_registry(entries: &[(&str, &str)]) -> Arc<dyn SkillRegistry> {
        skill_registry_with_fs(entries).1
    }

    /// Like [`skill_registry`], but also hands back the backing [`MemFs`] so a
    /// test can add/remove skills after construction and exercise `reload`.
    pub(crate) fn skill_registry_with_fs(
        entries: &[(&str, &str)],
    ) -> (MemFs, Arc<dyn SkillRegistry>) {
        let fs = MemFs::new();
        for (id, description) in entries {
            write_skill(&fs, id, description);
        }
        let registry = Arc::new(FsSkillRegistry::scan(fs.clone(), "skills").unwrap());
        (fs, registry)
    }

    /// The tool named `name` in `group` (cloned), panicking if absent.
    pub(crate) fn tool_named(group: &ToolGroup, name: &str) -> Tool {
        group
            .tools()
            .iter()
            .find(|tool| tool.name() == name)
            .unwrap()
            .clone()
    }
}
