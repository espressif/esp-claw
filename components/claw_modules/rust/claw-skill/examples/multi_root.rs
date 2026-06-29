//! Scan several skills roots at once, and see how a clashing id is rejected.
//!
//! Run with: `cargo run --example multi_root --target x86_64-unknown-linux-gnu`
//!
//! Mirrors the firmware layout: firmware-baked skills under one root and
//! user-installed skills under another. Ids are unique across *all* roots —
//! the same id in two roots is a hard [`SkillError::DuplicateId`], never a
//! silent override.

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillError, SkillId, SkillRegistry};

fn skill_md(description: &str) -> Vec<u8> {
    format!("---\n{{\"description\":\"{description}\"}}\n---\n# body\n").into_bytes()
}

fn main() -> anyhow::Result<()> {
    // Two distinct roots, each contributing different skills.
    let fs = MemFs::new();
    fs.write_atomic("system/time/SKILL.md", &skill_md("Built-in time helper."))?;
    fs.write_atomic(
        "data/notes/SKILL.md",
        &skill_md("User-installed notes skill."),
    )?;

    let registry = FsSkillRegistry::scan_roots(fs, ["system", "data"])?;
    println!("== merged catalog from system + data ==");
    for metadata in registry.catalog().entries() {
        println!("{:<8} {}", metadata.id().as_str(), metadata.description());
    }

    // Now provoke a collision: the same id `time` exists in both roots.
    let clashing = MemFs::new();
    clashing.write_atomic("system/time/SKILL.md", &skill_md("baked"))?;
    clashing.write_atomic("data/time/SKILL.md", &skill_md("installed"))?;

    println!("\n== scanning roots with a clashing id ==");
    match FsSkillRegistry::scan_roots(clashing, ["system", "data"]) {
        Err(SkillError::DuplicateId(id)) => {
            println!("rejected duplicate id: {id}");
            assert_eq!(id, SkillId::new("time"));
        }
        Err(other) => println!("unexpected error: {other}"),
        Ok(_) => println!("unexpected success — duplicate id should have been rejected"),
    }

    Ok(())
}
