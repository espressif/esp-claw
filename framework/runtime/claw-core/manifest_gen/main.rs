//! Build script for `claw_core` (entry point; configured via `build =
//! "manifest_gen/main.rs"` in `Cargo.toml`).
//!
//! This is a thin wiring layer: it resolves the build environment and then calls
//! each self-contained generator:
//! - [`agent_manifests`] turns `resources/agents/<kind>/` into typed
//!   `AgentManifest` statics in `$OUT_DIR/manifests.rs` (`include!`-d by
//!   `agent::manifest`).
//! - `claw_tool::bake` validates the `resources/tools/<function.name>/` layout
//!   (`schema.json` + `usage.md`) that the `tool_metadata!` macro `include_str!`s.
//!   The validator lives with the tool runtime.
//!
//! To add another generator, give it its own module exposing a `generate(...)`
//! entry and call it here; keep this function free of generation logic.
//!
//! Supporting modules: [`model`] (serde shapes), [`parse`] (read + validate), and
//! [`codegen`] (render Rust source) back the agent-manifest generator.

mod agent_manifests;
mod codegen;
mod model;
mod parse;

use std::env;
use std::path::PathBuf;

use anyhow::Result;

fn main() -> Result<()> {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR")?);
    let out_dir = PathBuf::from(env::var("OUT_DIR")?);

    // Re-run when any of the build script's own sources change. (Each generator
    // additionally registers the resource files it reads.)
    for source in [
        "manifest_gen/main.rs",
        "manifest_gen/agent_manifests.rs",
        "manifest_gen/model.rs",
        "manifest_gen/parse.rs",
        "manifest_gen/codegen.rs",
    ] {
        println!("cargo:rerun-if-changed={source}");
    }

    // Generators run as independent steps; add more calls here as needed.
    agent_manifests::generate(&manifest_dir, &out_dir)?;
    // The tool-directory contract is enforced by the tool runtime.
    claw_tool::bake::validate_tools_dir(&manifest_dir.join("resources/tools"))?;

    Ok(())
}
