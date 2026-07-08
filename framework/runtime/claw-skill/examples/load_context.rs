//! Activate skills through a [`SkillSet`] and return one-shot document content.
//!
//! Run with: `cargo run --example load_context --target x86_64-unknown-linux-gnu`

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillId};

fn skill_md(id: &str, description: &str, body: &str) -> Vec<u8> {
    format!(
        "---\n{{\"name\":\"{id}\",\"description\":\"{description}\",\"metadata\":{{\"manage_mode\":\"readonly\"}}}}\n---\n{body}"
    )
    .into_bytes()
}

fn main() -> anyhow::Result<()> {
    MemFs::new();
    MemFs::write_atomic(
        "skills/board_hardware_info/SKILL.md",
        &skill_md(
            "board_hardware_info",
            "Board GPIO and peripheral reference.",
            "# Board hardware\nGPIO map ...",
        ),
    )?;
    MemFs::write_atomic(
        "skills/light_switch/SKILL.md",
        &skill_md(
            "light_switch",
            "Control board lights.",
            "# Light switch\nCall the light capability ...",
        ),
    )?;

    let registry = Arc::new(FsSkillRegistry::<MemFs>::new().set_root("skills")?);
    let mut set = registry.skill_set();

    println!("== catalog context ==\n{}", set.catalog_context());

    let document = set.activate_skill(&SkillId::new("light_switch"))?;
    println!("== activated document ==\n{}", document.content());

    Ok(())
}
