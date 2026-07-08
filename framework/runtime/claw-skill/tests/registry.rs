//! Registry + skill-set tests over a hermetic in-memory `ClawFs` (`MemFs`).

#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillError, SkillId};

fn skill_md(id: &str, description: &str, body: &str) -> Vec<u8> {
    format!(
        "---\n{{\"name\":\"{id}\",\"description\":\"{description}\",\"metadata\":{{\"manage_mode\":\"readonly\"}}}}\n---\n{body}"
    )
    .into_bytes()
}

fn two_skill_fs() {
    MemFs::new();
    MemFs::write_atomic(
        "skills/alpha/SKILL.md",
        &skill_md(
            "alpha",
            "Alpha skill",
            "# Alpha\nRun {CUR_SKILL_DIR}/scripts/a.lua\n",
        ),
    )
    .unwrap();
    MemFs::write_atomic(
        "skills/beta/SKILL.md",
        &skill_md("beta", "Beta skill", "# Beta\nBeta body\n"),
    )
    .unwrap();
}

fn registry(root: &str) -> Arc<FsSkillRegistry<MemFs>> {
    two_skill_fs();
    Arc::new(FsSkillRegistry::<MemFs>::new().set_root(root).unwrap())
}

#[test]
fn set_root_builds_catalog_sorted_by_id() {
    let registry = registry("skills");
    let mut set = registry.skill_set();
    let catalog = set.catalog_context().to_string();
    assert!(catalog.starts_with("Available skills:\n"));
    assert!(
        catalog.find("- alpha: Alpha skill").unwrap() < catalog.find("- beta: Beta skill").unwrap()
    );
}

#[test]
fn list_skill_returns_json_catalog() {
    let registry = registry("skills");
    let mut set = registry.skill_set();
    let catalog = set.list_skill().unwrap().to_string();
    assert!(catalog.starts_with('['));
    assert!(catalog.contains(r#""id":"alpha""#));
    assert!(catalog.contains(r#""name":"beta""#));
    assert!(catalog.contains(r#""manage_mode":"readonly""#));
}

#[test]
fn activate_skill_strips_front_matter_expands_cur_skill_dir_and_wraps_xml() {
    let registry = registry("skills");
    let mut set = registry.skill_set();
    let document = set.activate_skill(&SkillId::new("alpha")).unwrap();
    let content = document.content();
    assert!(content.starts_with(r#"<skill_content name="alpha">"#));
    assert!(!content.contains("---"), "front-matter not stripped");
    assert!(content.contains("# Alpha"), "body missing");
    assert!(
        content.contains("skills/alpha/scripts/a.lua"),
        "placeholder must expand to the skill directory"
    );
    assert!(
        !content.contains("{CUR_SKILL_DIR}"),
        "placeholder must not be left in activated documents"
    );
    assert!(content.ends_with("</skill_content>"));
}

#[test]
fn earlier_root_shadows_later_duplicate_id() {
    MemFs::new();
    MemFs::write_atomic(
        "data/shared/SKILL.md",
        &skill_md("shared", "from data", "# Data"),
    )
    .unwrap();
    MemFs::write_atomic(
        "system/shared/SKILL.md",
        &skill_md("shared", "from system", "# System"),
    )
    .unwrap();
    let registry = Arc::new(
        FsSkillRegistry::<MemFs>::new()
            .set_root("data")
            .unwrap()
            .set_root("system")
            .unwrap(),
    );
    let mut set = registry.skill_set();
    assert!(set.catalog_context().contains("- shared: from data"));
    let document = set.activate_skill(&SkillId::new("shared")).unwrap();
    assert!(document.content().contains("# Data"));
    assert!(!document.content().contains("# System"));
}

#[test]
fn missing_roots_are_skipped() {
    two_skill_fs();
    let registry = Arc::new(
        FsSkillRegistry::<MemFs>::new()
            .set_root("missing")
            .unwrap()
            .set_root("skills")
            .unwrap(),
    );
    let mut set = registry.skill_set();
    let catalog = set.list_skill().unwrap().to_string();
    assert!(catalog.contains(r#""id":"alpha""#));
    assert!(catalog.contains(r#""id":"beta""#));
}

#[test]
fn activating_unknown_skill_is_not_found() {
    let registry = registry("skills");
    let mut set = registry.skill_set();
    let error = set.activate_skill(&SkillId::new("missing")).unwrap_err();
    assert!(matches!(error, SkillError::NotFound(_)));
}

#[test]
fn reload_picks_up_a_newly_added_skill() {
    let registry = registry("skills");
    let mut set = registry.skill_set();
    assert!(!set.catalog_context().contains("gamma"));

    MemFs::write_atomic(
        "skills/gamma/SKILL.md",
        &skill_md("gamma", "Gamma skill", "# Gamma"),
    )
    .unwrap();
    assert!(!set.catalog_context().contains("gamma"));

    set.reload().unwrap();
    assert!(set.catalog_context().contains("- gamma: Gamma skill"));
}
