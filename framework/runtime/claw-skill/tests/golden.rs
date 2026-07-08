//! Data-driven tests for the skill registry over real `SKILL.md` fixtures.
//!
//! Run with `CLAW_UPDATE_GOLDEN=1` to regenerate the golden files.

#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::path::{Path, PathBuf};
use std::sync::Arc;

use claw_interface::DiskFs;
use claw_skill::{FsSkillRegistry, SkillId};
use serde_json::Value;

const SKILLS_ROOT: &str = "skills";

fn data_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/data")
}

fn expected_dir() -> PathBuf {
    data_dir().join("skills_expected")
}

fn update_golden() -> bool {
    std::env::var_os("CLAW_UPDATE_GOLDEN").is_some()
}

fn registry() -> Arc<FsSkillRegistry<DiskFs>> {
    DiskFs::rooted(data_dir());
    Arc::new(
        FsSkillRegistry::<DiskFs>::new()
            .set_root(SKILLS_ROOT)
            .expect("scan skills fixtures"),
    )
}

fn catalog_json(registry: &Arc<FsSkillRegistry<DiskFs>>) -> String {
    let mut set = registry.skill_set();
    let mut rendered = set.list_skill().expect("render catalog").to_string();
    rendered.push('\n');
    rendered
}

fn catalog_ids(catalog: &str) -> Vec<String> {
    let value: Value = serde_json::from_str(catalog).expect("catalog json");
    value
        .as_array()
        .expect("catalog array")
        .iter()
        .map(|entry| {
            entry
                .get("id")
                .and_then(Value::as_str)
                .expect("catalog id")
                .to_string()
        })
        .collect()
}

fn assert_golden(path: &Path, actual: &str, label: &str) {
    if update_golden() {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).expect("create golden dir");
        }
        std::fs::write(path, actual).expect("write golden");
        return;
    }
    let expected = std::fs::read_to_string(path).unwrap_or_else(|_| {
        panic!(
            "missing golden for {label}: {} - run with CLAW_UPDATE_GOLDEN=1 to generate",
            path.display()
        )
    });
    assert_eq!(
        actual,
        &expected,
        "{label} does not match golden {}",
        path.display()
    );
}

#[test]
fn catalog_matches_golden() {
    let registry = registry();
    let catalog = catalog_json(&registry);
    assert_ne!(catalog, "[]\n", "no skills scanned from tests/data/skills");
    assert_golden(&expected_dir().join("catalog.json"), &catalog, "catalog");
}

#[test]
fn documents_match_golden() {
    let registry = registry();
    let catalog = catalog_json(&registry);
    let mut set = registry.skill_set();
    for id in catalog_ids(&catalog) {
        let document = set
            .activate_skill(&SkillId::new(id.clone()))
            .expect("activate skill document");
        assert!(
            !document.content().contains("\n---\n"),
            "front-matter not stripped for {id}"
        );
        assert_golden(
            &expected_dir().join(&id).join("document.md"),
            document.content(),
            &format!("document for {id}"),
        );
    }
}

#[test]
fn skill_set_activates_fixture_documents() {
    let registry = registry();
    let catalog = catalog_json(&registry);
    let first = catalog_ids(&catalog)
        .into_iter()
        .next()
        .expect("at least one fixture skill");

    let mut set = registry.skill_set();
    let document = set
        .activate_skill(&SkillId::new(first.clone()))
        .expect("activate skill");
    assert!(
        document.content().contains(&first),
        "activated document omits the skill id"
    );
}

#[test]
fn activating_unknown_skill_is_not_found() {
    let registry = registry();
    let mut set = registry.skill_set();
    assert!(set.activate_skill(&SkillId::new("does_not_exist")).is_err());
}
