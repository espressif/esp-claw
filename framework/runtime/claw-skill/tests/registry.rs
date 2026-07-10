#![allow(clippy::unwrap_used)]

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillError, SkillId, SkillManageMode};

#[test]
fn registry_parses_master_front_matter_shape() {
    let _fs = MemFs::new();
    write_skill("x", &skill_md("x"));

    let registry = Arc::new(FsSkillRegistry::<MemFs>::new().set_root("skills").unwrap());
    let mut skills = registry.skill_set();
    let catalog: serde_json::Value = serde_json::from_str(skills.list_skill().unwrap()).unwrap();
    let rows = catalog.as_array().unwrap();

    assert_eq!(rows.len(), 1);
    assert_eq!(rows[0]["id"], "x");
    assert_eq!(rows[0]["name"], "x");
    assert_eq!(rows[0]["description"], "d");
    assert_eq!(rows[0]["metadata"]["manage_mode"], "readonly");
    assert_eq!(rows[0]["file"], "x/SKILL.md");

    let document = skills.activate_skill(&SkillId::new("x")).unwrap();
    assert_eq!(
        document.content(),
        "<skill_content name=\"x\">\nbody\n</skill_content>"
    );
}

#[test]
fn missing_opening_fence_errors() {
    let _fs = MemFs::new();
    write_skill("x", "no front matter");

    let error = registry_error();
    assert!(matches!(error, SkillError::MissingOpeningFence(_)));
}

#[test]
fn missing_close_fence_errors() {
    let _fs = MemFs::new();
    write_skill("x", "---\n{}\n");

    let error = registry_error();
    assert!(matches!(error, SkillError::MissingClosingFence(_)));
}

#[test]
fn invalid_json_errors() {
    let _fs = MemFs::new();
    write_skill("x", "---\nnot json\n---\nbody");

    let error = registry_error();
    assert!(matches!(error, SkillError::InvalidJson(_, _)));
}

#[test]
fn front_matter_name_must_match_directory() {
    let _fs = MemFs::new();
    write_skill("x", &skill_md("other"));

    let error = registry_error();
    assert!(matches!(error, SkillError::InvalidFrontmatter(_, _)));
}

#[test]
fn skill_manage_mode_uses_canonical_labels_and_aliases() {
    let label: &'static str = SkillManageMode::Readonly.into();
    assert_eq!(label, "readonly");
    assert_eq!(
        SkillManageMode::try_from("readonly"),
        Ok(SkillManageMode::Readonly)
    );
    assert_eq!(
        SkillManageMode::try_from("web"),
        Ok(SkillManageMode::Readonly)
    );
    assert_eq!(
        SkillManageMode::try_from("runtime"),
        Ok(SkillManageMode::Runtime)
    );
}

fn write_skill(id: &str, document: &str) {
    MemFs::write_atomic(&format!("skills/{id}/SKILL.md"), document.as_bytes()).unwrap();
}

fn registry_error() -> SkillError {
    match FsSkillRegistry::<MemFs>::new().set_root("skills") {
        Ok(_) => panic!("registry load should fail"),
        Err(error) => error,
    }
}

fn skill_md(name: &str) -> String {
    format!(
        "---\n{{\"name\":\"{name}\",\"description\":\"d\",\"metadata\":{{\"manage_mode\":\"web\"}}}}\n---\nbody"
    )
}
