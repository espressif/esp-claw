//! Scan a skills directory and render the available-skills catalog.
//!
//! Run with: `cargo run --example catalog --target x86_64-unknown-linux-gnu`
//!
//! Uses an in-memory [`MemFs`] so the example is self-contained — in the
//! firmware the same [`FsSkillRegistry`] is configured over on-device `ClawFs`.

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::FsSkillRegistry;

/// Build a `SKILL.md` with a JSON front-matter header and a markdown body.
fn skill_md(id: &str, description: &str, body: &str) -> Vec<u8> {
    format!(
        "---\n{{\"name\":\"{id}\",\"description\":\"{description}\",\"metadata\":{{\"manage_mode\":\"readonly\"}}}}\n---\n{body}"
    )
    .into_bytes()
}

fn main() -> anyhow::Result<()> {
    // Lay out two skills under the `skills` root.
    MemFs::new();
    MemFs::write_atomic(
        "skills/weather_search/SKILL.md",
        &skill_md(
            "weather_search",
            "Answer weather and forecast questions via web search.",
            "# Weather\n...",
        ),
    )?;
    MemFs::write_atomic(
        "skills/light_switch/SKILL.md",
        &skill_md(
            "light_switch",
            "Turn board lights and LED strips on or off.",
            "# Light switch\n...",
        ),
    )?;

    let registry = Arc::new(FsSkillRegistry::<MemFs>::new().set_root("skills")?);
    let mut set = registry.skill_set();

    println!("== JSON catalog ==");
    println!("{}", set.list_skill()?);

    println!("\n== prompt catalog ==");
    print!("{}", set.catalog_context());

    Ok(())
}
