//! Load skills into a [`SkillSet`] and assemble their prompt context.
//!
//! Run with: `cargo run --example load_context --target x86_64-unknown-linux-gnu`
//!
//! Shows the runtime-mutable side of a skill set: load individually or by
//! group, read the cached `context()` fragment, then unload.

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillGroup, SkillId, SkillRegistry, SkillSet};

fn skill_md(description: &str, body: &str) -> Vec<u8> {
    format!("---\n{{\"description\":\"{description}\"}}\n---\n{body}").into_bytes()
}

fn main() -> anyhow::Result<()> {
    let fs = MemFs::new();
    fs.write_atomic(
        "skills/board_hardware_info/SKILL.md",
        &skill_md(
            "Board GPIO and peripheral reference.",
            "# Board hardware\nGPIO map ...",
        ),
    )?;
    fs.write_atomic(
        "skills/light_switch/SKILL.md",
        &skill_md(
            "Control board lights.",
            "# Light switch\nCall the light capability ...",
        ),
    )?;

    let registry: Arc<dyn SkillRegistry> = Arc::new(FsSkillRegistry::scan(fs, "skills")?);
    let mut set = SkillSet::new(registry);

    // Nothing loaded yet: the context fragment is empty.
    println!("empty? {}", set.is_empty());

    // Load one skill directly, and a related pair as a named group.
    set.load("manual", SkillId::new("light_switch"))?;
    set.load_group(SkillGroup::new(
        "hardware",
        [SkillId::new("board_hardware_info")],
    ))?;

    // `context()` assembles the loaded bodies; the result is cached until the
    // loaded set changes, so repeated reads are O(1).
    println!("\n== assembled context ==\n{}", set.context()?);

    // Unload and the fragment shrinks back.
    set.unload(&SkillId::new("light_switch"));
    println!("== after unloading light_switch ==\n{}", set.context()?);

    Ok(())
}
