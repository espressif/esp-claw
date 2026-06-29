//! Data-driven tests for the skill registry over real `SKILL.md` fixtures.
//!
//! - Input fixtures:  `tests/data/skills/<id>/SKILL.md` (+ bundled scripts)
//! - Golden expected: `tests/data/skills_expected/catalog.json` and
//!   `tests/data/skills_expected/<id>/document.md`
//!
//! The registry is scanned over a `DiskFs` rooted at `tests/data` with the
//! virtual root `skills`. Document bodies are returned verbatim (front-matter
//! stripped) — there is no `{CUR_SKILL_DIR}` expansion, so any such placeholder
//! is preserved literally in the golden.
//!
//! The whole skills folder is discovered dynamically (nothing is hard-coded per
//! skill), so dropping a new skill into `tests/data/skills/` and regenerating the
//! goldens is all it takes to cover it.
//!
//! Run with `CLAW_UPDATE_GOLDEN=1` to (re)generate the golden files.

#![allow(clippy::unwrap_used, clippy::expect_used)]

use std::path::{Path, PathBuf};
use std::sync::Arc;

use claw_interface::DiskFs;
use claw_skill::{FsSkillRegistry, SkillId, SkillRegistry, SkillSet};
use serde_json::json;

/// Virtual skills root handed to the registry; maps onto `tests/data/skills`.
const SKILLS_ROOT: &str = "skills";

// The registry is scanned over the shared `DiskFs::rooted(base)` (the `diskfs`
// dev-dependency feature) so virtual paths like `skills/<id>` resolve under
// `tests/data` and stay portable.

// ---------------------------------------------------------------------------
// Fixture paths + helpers
// ---------------------------------------------------------------------------

fn data_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/data")
}

fn expected_dir() -> PathBuf {
    data_dir().join("skills_expected")
}

fn update_golden() -> bool {
    std::env::var_os("CLAW_UPDATE_GOLDEN").is_some()
}

fn registry() -> FsSkillRegistry<DiskFs> {
    FsSkillRegistry::scan(DiskFs::rooted(data_dir()), SKILLS_ROOT).expect("scan skills fixtures")
}

/// The catalog rendered as canonical pretty JSON (trailing newline) for golden
/// comparison.
fn catalog_json(registry: &FsSkillRegistry<DiskFs>) -> String {
    let entries: Vec<_> = registry
        .catalog()
        .entries()
        .iter()
        .map(|metadata| {
            json!({
                "id": metadata.id().as_str(),
                "description": metadata.description(),
            })
        })
        .collect();
    let mut rendered = serde_json::to_string_pretty(&json!(entries)).expect("serialize catalog");
    rendered.push('\n');
    rendered
}

/// Assert `actual` equals the golden at `path`, or write it when updating.
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
            "missing golden for {label}: {} — run with CLAW_UPDATE_GOLDEN=1 to generate",
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

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[test]
fn catalog_matches_golden() {
    let registry = registry();
    assert!(
        !registry.catalog().entries().is_empty(),
        "no skills scanned from tests/data/skills"
    );
    assert_golden(
        &expected_dir().join("catalog.json"),
        &catalog_json(&registry),
        "catalog",
    );
}

#[test]
fn documents_match_golden() {
    let registry = registry();
    for metadata in registry.catalog().entries() {
        let id = metadata.id();
        let document = registry.document(id).expect("read skill document");
        assert!(
            !document.starts_with("---"),
            "front-matter not stripped for {id}"
        );
        assert_golden(
            &expected_dir().join(id.as_str()).join("document.md"),
            &document,
            &format!("document for {id}"),
        );
    }
}

#[test]
fn skill_set_loads_unloads_and_caches() {
    let shared: Arc<dyn SkillRegistry> = Arc::new(registry());
    // Pick the first fixture skill dynamically rather than hard-coding an id.
    let first = shared
        .catalog()
        .entries()
        .first()
        .expect("at least one fixture skill")
        .id()
        .clone();

    let mut set = SkillSet::new(Arc::clone(&shared));
    assert!(set.context().expect("empty context").is_empty());

    set.load("test", first.clone()).expect("load skill");
    let loaded = set.context().expect("loaded context").to_string();
    assert!(
        loaded.contains(first.as_str()),
        "loaded context omits the skill id"
    );
    // A second read with nothing changed returns the same cached content.
    assert_eq!(set.context().expect("cached context"), loaded);

    set.unload(&first);
    assert!(set.context().expect("post-unload context").is_empty());
}

#[test]
fn loading_unknown_skill_is_not_found() {
    let shared: Arc<dyn SkillRegistry> = Arc::new(registry());
    let mut set = SkillSet::new(shared);
    assert!(set.load("test", SkillId::new("does_not_exist")).is_err());
}
