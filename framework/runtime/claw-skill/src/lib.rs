//! Skills: per-agent, dynamically loadable prompt context.
//!
//! - [`SkillRegistry`] / [`FsSkillRegistry`] — the catalog source: scans one or
//!   more skills directories, reads each `SKILL.md`'s front-matter for the
//!   catalog, and reads full documents on demand. Skill ids are unique across
//!   all roots; a collision is a hard error ([`SkillError::DuplicateId`]).
//! - Skill domain types — [`SkillId`], [`SkillMetadata`].
//! - [`SkillSet`] / [`SkillGroup`] — the agent's loaded skills plus two
//!   dirty-cached, borrowed prompt fragments: [`catalog`](SkillSet::catalog)
//!   (every available skill as a menu) and [`context`](SkillSet::context) (the
//!   full bodies of the skills currently loaded). Mutable at runtime (load /
//!   unload without restarting the agent).

mod registry;
mod skill;
mod skill_set;

pub use registry::{CatalogSnapshot, FsSkillRegistry, SkillRegistry};
pub use skill::{SkillError, SkillId, SkillMetadata};
pub use skill_set::{SkillGroup, SkillSet};
