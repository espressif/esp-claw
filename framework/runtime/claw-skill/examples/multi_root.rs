//! Scan several skills roots at once, and see how root priority resolves a
//! clashing id.
//!
//! Run with: `cargo run --example multi_root --target x86_64-unknown-linux-gnu`
//!
//! Mirrors the firmware layout: user-installed skills under the writable DATA
//! root can shadow firmware-baked skills under the read-only SYSTEM root.

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillId};

fn skill_md(id: &str, description: &str) -> Vec<u8> {
    format!(
        "---\n{{\"name\":\"{id}\",\"description\":\"{description}\",\"metadata\":{{\"manage_mode\":\"readonly\"}}}}\n---\n# body\n"
    )
    .into_bytes()
}

fn main() -> anyhow::Result<()> {
    // Two distinct roots, each contributing different skills.
    MemFs::new();
    MemFs::write_atomic(
        "system/time/SKILL.md",
        &skill_md("time", "Built-in time helper."),
    )?;
    MemFs::write_atomic(
        "data/notes/SKILL.md",
        &skill_md("notes", "User-installed notes skill."),
    )?;

    let registry = std::sync::Arc::new(
        FsSkillRegistry::<MemFs>::new()
            .set_root("data")?
            .set_root("system")?,
    );
    let mut set = registry.skill_set();
    println!("== merged catalog from data + system ==");
    print!("{}", set.catalog_context());

    // Now use a collision: the same id `time` exists in both roots, and the
    // earlier DATA root shadows the later SYSTEM root.
    MemFs::new();
    MemFs::write_atomic("system/time/SKILL.md", &skill_md("time", "baked"))?;
    MemFs::write_atomic("data/time/SKILL.md", &skill_md("time", "installed"))?;

    println!("\n== scanning roots with a clashing id ==");
    let registry = std::sync::Arc::new(
        FsSkillRegistry::<MemFs>::new()
            .set_root("data")?
            .set_root("system")?,
    );
    let mut set = registry.skill_set();
    let catalog = set.catalog_context().to_string();
    println!("{catalog}");
    assert!(catalog.contains("- time: installed"));
    let document = set.activate_skill(&SkillId::new("time"))?;
    assert!(document.content().contains("# body"));

    Ok(())
}
