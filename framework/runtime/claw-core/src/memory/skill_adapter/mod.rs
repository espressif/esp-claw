//! Skill context adapter.
//!
//! This adapter owns the runtime [`SkillSet`] source for an agent. It projects
//! the skill catalog into `BlockKind::SkillList` and exposes skill tools that
//! read from the same buffered source.

mod tools;

use std::sync::{Arc, Mutex, MutexGuard};

use claw_context::{Block, BlockKind, ContextSink};
use claw_skill::SkillSet;
use claw_tool::Tool;

use super::traits::{ContextAdapter, ContextAdapterInput};

const ADAPTER_ID: &str = "skills";
pub(crate) struct SkillContextAdapter {
    skills: Arc<Mutex<SkillSet>>,
}

impl SkillContextAdapter {
    pub(crate) fn new(skills: SkillSet) -> Self {
        Self {
            skills: Arc::new(Mutex::new(skills)),
        }
    }
}

impl ContextAdapter for SkillContextAdapter {
    fn id(&self) -> &str {
        ADAPTER_ID
    }

    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        let mut skills = lock_skill_set(&self.skills);
        let rendered = skills.catalog_context();
        output.block(Block::new(BlockKind::SkillList, rendered));
    }

    fn tools(&self) -> Vec<Tool> {
        tools::skill_tools(Arc::clone(&self.skills))
    }
}

pub(super) fn lock_skill_set(skills: &Mutex<SkillSet>) -> MutexGuard<'_, SkillSet> {
    skills.lock().unwrap_or_else(|poison| poison.into_inner())
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
pub(crate) mod test_support {
    use std::sync::{Arc, Mutex};

    use claw_interface::{ClawFs, MemFs};
    use claw_skill::{FsSkillRegistry, SkillRegistry, SkillSet};
    use claw_tool::Tool;

    use super::tools::skill_tools;

    pub(crate) fn skill_tools_for_test(skills: SkillSet) -> Vec<Tool> {
        skill_tools(Arc::new(Mutex::new(skills)))
    }

    /// Write a minimal `SKILL.md` for `id` under the `skills` root of `fs`, so
    /// both catalog scans and document reads succeed.
    pub(crate) fn write_skill(_fs: &MemFs, id: &str, description: &str) {
        let document = format!(
            "---\n{{\"name\":\"{id}\",\"description\":\"{description}\",\"metadata\":{{\"manage_mode\":\"readonly\"}}}}\n---\n# {id}\n\nBody for {id}.\n"
        );
        MemFs::write_atomic(&format!("skills/{id}/SKILL.md"), document.as_bytes()).unwrap();
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
        let registry = Arc::new(FsSkillRegistry::<MemFs>::new().set_root("skills").unwrap());
        (fs, registry)
    }

    /// The tool named `name` in `tools` (cloned), panicking if absent.
    pub(crate) fn tool_named(tools: &[Tool], name: &str) -> Tool {
        tools
            .iter()
            .find(|tool| tool.name() == name)
            .unwrap()
            .clone()
    }
}
