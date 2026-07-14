//! The **agent-manifest** generator.
//!
//! Self-contained codegen step: reads `resources/agents/<kind>/`, parses +
//! validates each kind, and writes the typed `ENTRIES: &[AgentCatalogEntry]`
//! array to `<out_dir>/manifests.rs`.
//!
//! The whole step is sealed behind the single entry point [`generate`]; `main`
//! only calls it. Other generators (if added) live in their own sibling modules
//! with the same shape, so each stays isolated and `main` stays a thin wiring
//! layer.

use std::fs;
use std::path::Path;

use anyhow::{anyhow, bail, Context, Result};

use crate::codegen;
use crate::parse::{
    parse_common, parse_kind, CommonBase, ParsedKind, ParsedManifest, COMMON_FILES, MANIFEST_FILES,
};

/// The generated file's name within `OUT_DIR`.
const OUTPUT_FILE: &str = "manifests.rs";

/// Reserved directory under `resources/agents/` for data shared across kinds
/// (e.g. a shared instruction preamble). It is not an agent kind, so the
/// generator skips it rather than parsing it as a manifest.
const SHARED_DIR: &str = "common";

/// Generate the agent catalog.
///
/// Reads `<manifest_dir>/resources/agents`, parses and validates every kind
/// directory, and writes `<out_dir>/manifests.rs`. Registers the resources it
/// reads for `rerun-if-changed` so edits re-trigger codegen.
///
/// # Errors
///
/// Returns an error if the resources directory cannot be read, a manifest is
/// malformed/invalid (via [`parse_kind`]), no kinds are found, or the output
/// file cannot be written — any of which fails the build.
pub(crate) fn generate(manifest_dir: &Path, out_dir: &Path) -> Result<()> {
    let agents_dir = manifest_dir.join("resources/agents");
    // Re-run when a kind is added or removed.
    println!("cargo:rerun-if-changed={}", agents_dir.display());

    // The shared base every kind inherits. Tracked for rerun (including
    // agent.json, so adding one re-triggers the build and fails it).
    let common_dir = agents_dir.join(SHARED_DIR);
    for file in COMMON_FILES {
        println!("cargo:rerun-if-changed={}", common_dir.join(file).display());
    }
    let common = parse_common(&common_dir)?;

    // Every kind inherits the common base: its own entries extend the base.
    let mut kinds: Vec<ParsedManifest> = collect_kinds(&agents_dir)?
        .into_iter()
        .map(|kind| inherit_base(kind, &common))
        .collect();
    // Deterministic output regardless of directory iteration order.
    kinds.sort_by(|left, right| left.kind.cmp(&right.kind));

    if kinds.is_empty() {
        bail!("no agent kinds found under {}", agents_dir.display());
    }

    let generated = codegen::render(&kinds);
    let out_path = out_dir.join(OUTPUT_FILE);
    fs::write(&out_path, generated).with_context(|| format!("writing {}", out_path.display()))?;

    Ok(())
}

/// Parse every kind subdirectory under `agents_dir`, registering each manifest
/// file for `rerun-if-changed`. Hidden directories and the reserved shared-data
/// folder are skipped; only proper kind directories carry a manifest.
fn collect_kinds(agents_dir: &Path) -> Result<Vec<ParsedKind>> {
    let mut kinds = Vec::new();
    for entry in
        fs::read_dir(agents_dir).with_context(|| format!("reading {}", agents_dir.display()))?
    {
        let entry = entry?;
        let path = entry.path();
        let name = path
            .file_name()
            .and_then(|name| name.to_str())
            .ok_or_else(|| anyhow!("non-UTF-8 entry name in {}", agents_dir.display()))?
            .to_string();

        // Hidden entries (e.g. `.gitkeep`, `.DS_Store`) and the reserved shared
        // base are not kinds; skip them.
        if name.starts_with('.') || name == SHARED_DIR {
            continue;
        }
        // Everything else must be a proper kind directory: a stray file here
        // would otherwise be silently ignored, so reject it ("no more, no less").
        if !path.is_dir() {
            bail!(
                "{}: unexpected file '{name}' — only agent kind directories (and the \
                 reserved '{SHARED_DIR}' base) may live under resources/agents",
                agents_dir.display()
            );
        }

        for file in MANIFEST_FILES {
            println!("cargo:rerun-if-changed={}", path.join(file).display());
        }
        kinds.push(parse_kind(&path)?);
    }
    Ok(kinds)
}

/// Fold the shared `common` base into one kind: the base entries come first,
/// then the kind's own, with duplicates dropped so a kind can list a tool or
/// skill already in the base without it appearing twice. The shared
/// instructions preamble is recorded so codegen can prepend it to the kind's own
/// prompt.
fn inherit_base(kind: ParsedKind, common: &CommonBase) -> ParsedManifest {
    ParsedManifest {
        kind: kind.kind,
        description: kind.description,
        spawn_enabled: kind.spawn_enabled,
        allowed_kinds: kind.allowed_kinds,
        retries: kind.retries,
        tool_block_retries: kind.tool_block_retries,
        tool_groups: merge_unique(&common.tool_groups, &kind.tool_groups),
        skills: merge_unique(&common.skills, &kind.skills),
        instructions_path: kind.instructions_path,
        common_instructions_path: common.instructions_path.clone(),
    }
}

/// Concatenate `base` then `own`, preserving first-seen order and dropping later
/// duplicates.
fn merge_unique(base: &[String], own: &[String]) -> Vec<String> {
    let mut merged: Vec<String> = Vec::with_capacity(base.len() + own.len());
    for name in base.iter().chain(own) {
        if !merged.iter().any(|existing| existing == name) {
            merged.push(name.clone());
        }
    }
    merged
}
