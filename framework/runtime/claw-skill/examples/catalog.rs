//! Scan a skills directory and render the available-skills catalog.
//!
//! Run with: `cargo run --example catalog --target x86_64-unknown-linux-gnu`
//!
//! Uses an in-memory [`MemFs`] so the example is self-contained — in the
//! firmware the same [`FsSkillRegistry`] is scanned over the on-device `ClawFs`.

use std::sync::Arc;

use claw_interface::{ClawFs, MemFs};
use claw_skill::{FsSkillRegistry, SkillRegistry, SkillSet};

/// Build a `SKILL.md` with a JSON front-matter header and a markdown body.
fn skill_md(description: &str, body: &str) -> Vec<u8> {
    format!("---\n{{\"description\":\"{description}\"}}\n---\n{body}").into_bytes()
}

fn main() -> anyhow::Result<()> {
    // Lay out two skills under the `skills` root.
    let fs = MemFs::new();
    fs.write_atomic(
        "skills/weather_search/SKILL.md",
        &skill_md(
            "Answer weather and forecast questions via web search.",
            "# Weather\n...",
        ),
    )?;
    fs.write_atomic(
        "skills/light_switch/SKILL.md",
        &skill_md(
            "Turn board lights and LED strips on or off.",
            "# Light switch\n...",
        ),
    )?;

    // Scan once; the catalog is cheap in-memory metadata (no bodies read).
    let registry = FsSkillRegistry::scan(fs, "skills")?;

    println!("== structured catalog ==");
    for metadata in registry.catalog().entries() {
        println!("{:<14} {}", metadata.id().as_str(), metadata.description());
    }

    // The same data rendered as the prompt-facing menu the model sees.
    let mut set = SkillSet::new(Arc::new(registry));
    println!("\n== rendered menu (SkillSet::catalog) ==");
    print!("{}", set.catalog());

    Ok(())
}
