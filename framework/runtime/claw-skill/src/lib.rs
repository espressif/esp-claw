//! Filesystem-backed skills: catalog context plus one-shot document activation.
//!
//! [`FsSkillRegistry`] scans priority-ordered roots such as DATA then SYSTEM.
//! [`SkillSet`] is the per-agent projection that renders the catalog and
//! activates one `SKILL.md` document on demand. Activating a skill returns an
//! owned [`SkillDocument`]; it does not create persistent loaded-skill state.

mod registry;
mod skill;
mod skill_set;

pub use registry::{
    CatalogSnapshot, EmptySkillRegistry, FsSkillRegistry, SkillRegistry, SkillRegistryVersion,
};
pub use skill::{
    ParseSkillManageModeError, Skill, SkillDocument, SkillError, SkillFrontmatterMetadata, SkillId,
    SkillManageMode,
};
pub use skill_set::SkillSet;
