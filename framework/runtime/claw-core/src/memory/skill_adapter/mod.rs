//! Skill context adapter.
//!
//! This adapter owns the runtime [`SkillSet`] source for an agent. It projects
//! the skill catalog into `BlockKind::SkillList` and exposes skill tools that
//! read from the same buffered source.

mod tools;

use std::sync::{Arc, Mutex, MutexGuard};

use claw_context::{Block, BlockKind, ContextSink};
use claw_skill::SkillSet;
use claw_tool::ToolGroup;

use super::traits::ContextAdapter;

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
    fn contribute(&mut self, output: &mut ContextSink<'_>) {
        let mut skills = lock_skill_set(&self.skills);
        let rendered = skills.catalog_context();
        output.block(Block::new(BlockKind::SkillList, rendered));
    }

    fn tools(&self) -> Option<ToolGroup> {
        Some(tools::skill_tools(Arc::clone(&self.skills)))
    }
}

pub(super) fn lock_skill_set(skills: &Mutex<SkillSet>) -> MutexGuard<'_, SkillSet> {
    skills.lock().unwrap_or_else(|poison| poison.into_inner())
}
