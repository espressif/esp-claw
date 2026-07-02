//! Registry + skill-set tests over a hermetic in-memory `ClawFs` (`MemFs`).
//!
//! No on-disk fixtures: each test builds the exact skill tree it needs, so the
//! decided behaviours are pinned in one place — root-priority shadowing,
//! `{CUR_SKILL_DIR}` expansion, and the two borrowed prompt fragments.

#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillError, SkillId, SkillRegistry, SkillSet};

/// A `SKILL.md` with the given description and body.
fn skill_md(description: &str, body: &str) -> Vec<u8> {
    format!("---\n{{\"description\":\"{description}\"}}\n---\n{body}").into_bytes()
}

/// A `MemFs` with two skills (`alpha`, `beta`) under the `skills` root.
fn two_skill_fs() -> MemFs {
    let fs = MemFs::new();
    fs.write_atomic(
        "skills/alpha/SKILL.md",
        &skill_md(
            "Alpha skill",
            "# Alpha\nRun {CUR_SKILL_DIR}/scripts/a.lua\n",
        ),
    )
    .unwrap();
    fs.write_atomic(
        "skills/beta/SKILL.md",
        &skill_md("Beta skill", "# Beta\nBeta body\n"),
    )
    .unwrap();
    fs
}

#[test]
fn scan_builds_catalog_sorted_by_id() {
    let registry = FsSkillRegistry::scan(two_skill_fs(), "skills").unwrap();
    let catalog = registry.catalog();
    let ids: Vec<&str> = catalog
        .entries()
        .iter()
        .map(|metadata| metadata.id().as_str())
        .collect();
    assert_eq!(ids, ["alpha", "beta"]);
    assert_eq!(
        registry
            .metadata(&SkillId::new("alpha"))
            .unwrap()
            .description(),
        "Alpha skill"
    );
}

#[test]
fn document_strips_front_matter_and_expands_cur_skill_dir() {
    let registry = FsSkillRegistry::scan(two_skill_fs(), "skills").unwrap();
    let document = registry.document(&SkillId::new("alpha")).unwrap();
    assert!(!document.starts_with("---"), "front-matter not stripped");
    assert!(document.contains("# Alpha"), "body missing");
    assert!(
        document.contains("skills/alpha/scripts/a.lua"),
        "placeholder must expand to the skill directory"
    );
    assert!(
        !document.contains("{CUR_SKILL_DIR}"),
        "placeholder must not be left in loaded documents"
    );
}

#[test]
fn earlier_root_shadows_later_duplicate_id() {
    let fs = MemFs::new();
    fs.write_atomic("data/shared/SKILL.md", &skill_md("from data", "# Data"))
        .unwrap();
    fs.write_atomic(
        "system/shared/SKILL.md",
        &skill_md("from system", "# System"),
    )
    .unwrap();
    let registry = FsSkillRegistry::scan_roots(fs, ["data", "system"]).unwrap();
    assert_eq!(
        registry
            .metadata(&SkillId::new("shared"))
            .unwrap()
            .description(),
        "from data"
    );
    assert!(registry
        .document(&SkillId::new("shared"))
        .unwrap()
        .contains("# Data"));
}

#[test]
fn missing_roots_are_skipped() {
    let registry = FsSkillRegistry::scan_roots(two_skill_fs(), ["missing", "skills"]).unwrap();
    assert_eq!(registry.catalog().entries().len(), 2);
}

#[test]
fn document_of_unknown_skill_is_not_found() {
    let registry = FsSkillRegistry::scan(two_skill_fs(), "skills").unwrap();
    let error = registry.document(&SkillId::new("missing")).unwrap_err();
    assert!(matches!(error, SkillError::NotFound(_)));
}

#[test]
fn skill_set_catalog_lists_every_available_skill() {
    let registry: Arc<dyn SkillRegistry> =
        Arc::new(FsSkillRegistry::scan(two_skill_fs(), "skills").unwrap());
    let mut set = SkillSet::new(registry);
    let catalog = set.catalog().to_string();
    assert!(catalog.starts_with("Available skills:\n"));
    assert!(catalog.contains("- alpha: Alpha skill"));
    assert!(catalog.contains("- beta: Beta skill"));
}

#[test]
fn skill_set_context_loads_unloads_and_caches() {
    let registry: Arc<dyn SkillRegistry> =
        Arc::new(FsSkillRegistry::scan(two_skill_fs(), "skills").unwrap());
    let mut set = SkillSet::new(registry);
    assert!(set.context().unwrap().is_empty());

    set.load("test", SkillId::new("alpha")).unwrap();
    let loaded = set.context().unwrap().to_string();
    assert!(loaded.contains("## Skill: alpha (test)"));
    assert!(loaded.contains("# Alpha"));
    // Re-read with nothing changed returns the same cached content.
    assert_eq!(set.context().unwrap(), loaded);

    set.unload(&SkillId::new("alpha"));
    assert!(set.context().unwrap().is_empty());
}

#[test]
fn reload_picks_up_a_newly_added_skill() {
    let fs = two_skill_fs();
    let registry = FsSkillRegistry::scan(fs.clone(), "skills").unwrap();
    assert_eq!(registry.catalog().entries().len(), 2);

    fs.write_atomic("skills/gamma/SKILL.md", &skill_md("Gamma skill", "# Gamma"))
        .unwrap();
    // The pre-reload snapshot is unchanged; reload swaps in a fresh one.
    assert_eq!(registry.catalog().entries().len(), 2);
    registry.reload().unwrap();

    let catalog = registry.catalog();
    let ids: Vec<&str> = catalog
        .entries()
        .iter()
        .map(|metadata| metadata.id().as_str())
        .collect();
    assert_eq!(ids, ["alpha", "beta", "gamma"]);
}

#[test]
fn skill_set_catalog_reflects_registry_reload() {
    let fs = two_skill_fs();
    let registry: Arc<FsSkillRegistry<MemFs>> =
        Arc::new(FsSkillRegistry::scan(fs.clone(), "skills").unwrap());
    let mut set = SkillSet::new(registry.clone());
    assert!(!set.catalog().contains("gamma"));

    fs.write_atomic("skills/gamma/SKILL.md", &skill_md("Gamma skill", "# Gamma"))
        .unwrap();
    registry.reload().unwrap();
    // The cache is keyed on snapshot identity, so the reload invalidates it.
    assert!(set.catalog().contains("- gamma: Gamma skill"));
}

#[test]
fn loading_unknown_skill_is_not_found() {
    let registry: Arc<dyn SkillRegistry> =
        Arc::new(FsSkillRegistry::scan(two_skill_fs(), "skills").unwrap());
    let mut set = SkillSet::new(registry);
    assert!(set.load("test", SkillId::new("does_not_exist")).is_err());
}
