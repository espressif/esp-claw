//! Demonstrates assembling a request with `claw-context`.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-context --example build_context --target x86_64-unknown-linux-gnu
//! ```
//!
//! The crate owns *placement, change detection, and rendering*; this example
//! plays the role of the *content sources* (the "filling"). It shows that blocks
//! declared in any order render in the spec wire order, that a custom block slots
//! within its band, and that the context only re-renders when a block actually
//! changes (the `version()` stays put across an identical re-declaration, then
//! advances when a block is updated).

use std::borrow::Cow;

use claw_context::{Band, Block, BlockKind, Context, Scope};
use serde_json::json;

fn main() {
    let mut context = Context::new();

    // A custom always-on hardware-docs block, placed at the bottom of the agent
    // durable group (after ReasoningEffort, order 3). Query-specific retrieved
    // docs should normally arrive as tool results in `history`, not as durable blocks.
    let hardware_docs = Block::new(
        BlockKind::Custom {
            band: Band::Durable,
            scope: Scope::Agent,
            order: 4,
            label: Cow::Borrowed("HardwareDocs"),
        },
        "Doc: the GPIO API exposes claw_gpio_set_level(pin, level).",
    );

    // Declare the full working-mode context. Insertion order is deliberately
    // scrambled — the wire order is fixed by BlockKind.
    context
        .with(Block::new(
            BlockKind::OutputContract,
            "Respond as JSON: {actions, blockers, needs_approval, next_step}.",
        ))
        .with(Block::new(
            BlockKind::AgentInstruction,
            "You are Claw, a helpful on-device worker. Execute the task and report structured results.",
        ))
        .with(Block::new(
            BlockKind::AgentMemory,
            "Prefers metric units. Has an ESP32-S3 DevKitC.",
        ))
        .with(Block::new(
            BlockKind::SkillList,
            "Skill blink_led: toggle the on-board LED N times.",
        ))
        .with(Block::new(
            BlockKind::ModeFraming,
            "Task: blink the LED 3 times. Workspace: board=esp32s3.",
        ))
        .with(hardware_docs)
        .with(Block::new(
            BlockKind::RecentContext,
            "tool_result(claw_gpio_set_level): ok",
        ))
        // A reminder is the ephemeral tail, never persisted, after the history.
        .reminder(Some("Only the blink_led skill is permitted this phase."));

    let history = json!([{ "role": "user", "content": "Make the LED blink." }]);

    let version_before = context.version();
    let request = context.request(&history);
    println!(
        "===== system prefix (version {version_before}) =====\n{}\n",
        request.system()
    );
    println!("===== reminders (ephemeral tail) =====");
    for reminder in request.reminders() {
        if let Some(text) = reminder.get("content").and_then(serde_json::Value::as_str) {
            println!("{text}");
        }
    }

    // Re-declaring identical content every tick is a free no-op: the version (and
    // so the cached prefix) does not move.
    context.with(Block::new(
        BlockKind::SkillList,
        "Skill blink_led: toggle the on-board LED N times.",
    ));
    println!(
        "\nversion after identical re-declaration: {} (unchanged: {})",
        context.version(),
        context.version() == version_before
    );

    // A real change advances the version; the next request re-renders.
    context.with(Block::new(BlockKind::SkillList, ""));
    println!("version after unloading the skill: {}", context.version());
}
