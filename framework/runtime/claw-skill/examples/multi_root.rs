//! Scan several skills roots at once, and see how root priority resolves a
//! clashing id.
//!
//! Run with: `cargo run --example multi_root --target x86_64-unknown-linux-gnu`
//!
//! Mirrors the firmware layout: user-installed skills under the writable DATA
//! root can shadow firmware-baked skills under the read-only SYSTEM root.

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillId, SkillRegistry};

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

    let registry = FsSkillRegistry::scan_roots(fs, ["data", "system"])?;
    println!("== merged catalog from data + system ==");
    for metadata in registry.catalog().entries() {
        println!("{:<8} {}", metadata.id().as_str(), metadata.description());
    }

    // Now use a collision: the same id `time` exists in both roots, and the
    // earlier DATA root shadows the later SYSTEM root.
    let clashing = MemFs::new();
    clashing.write_atomic("system/time/SKILL.md", &skill_md("baked"))?;
    clashing.write_atomic("data/time/SKILL.md", &skill_md("installed"))?;

    println!("\n== scanning roots with a clashing id ==");
    let registry = FsSkillRegistry::scan_roots(clashing, ["data", "system"])?;
    let metadata = registry
        .metadata(&SkillId::new("time"))
        .ok_or_else(|| anyhow::anyhow!("shadowed skill is missing"))?;
    println!("time -> {}", metadata.description());
    assert_eq!(metadata.description(), "installed");

    Ok(())
}
