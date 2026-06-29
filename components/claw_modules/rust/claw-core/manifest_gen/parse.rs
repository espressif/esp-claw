//! Read and validate one `resources/agents/<kind>/` directory into a
//! [`ParsedManifest`]. Any malformed JSON, missing file, or kind/dir mismatch is
//! returned as an error so the build script can fail the build with a clear
//! message.

use std::fs;
use std::path::{Path, PathBuf};

use anyhow::{anyhow, bail, Context, Result};

use crate::model::{AgentJson, CapabilitiesJson, SkillsJson};

/// A fully-parsed, validated manifest — the build-time counterpart to the
/// runtime `AgentManifest`. Strings are owned here; [`crate::codegen`] renders
/// them into `&'static` data.
pub struct ParsedManifest {
    pub kind: String,
    pub description: String,
    pub spawn_enabled: bool,
    pub allowed_kinds: Vec<String>,
    pub retries: u32,
    pub tool_block_retries: Option<u32>,
    pub capabilities: Vec<String>,
    pub skills: Vec<String>,
    /// Absolute path to `instructions.md`, embedded via `include_str!` in the
    /// generated code so the bytes are not duplicated into the generated source.
    pub instructions_path: PathBuf,
    /// Absolute path to the shared `common/instructions.md` preamble, prepended
    /// (via `include_str!`) before this kind's own instructions at codegen.
    /// `None` until [`crate::agent_manifests::inherit_base`] folds the common base
    /// in; `Some` for every kind in the emitted output.
    pub common_instructions_path: Option<PathBuf>,
}

/// The manifest files expected in every kind directory; also the set the build
/// script registers for `rerun-if-changed`.
pub const MANIFEST_FILES: &[&str] = &[
    "agent.json",
    "capabilities/capabilities.json",
    "skills/skills.json",
    "instructions.md",
];

/// Files the shared `common/` base is tracked for `rerun-if-changed`. `agent.json`
/// is included so that *adding* one re-triggers the build (and fails it, since
/// the shared base must not declare an agent kind).
pub const COMMON_FILES: &[&str] = &[
    "capabilities/capabilities.json",
    "skills/skills.json",
    "instructions.md",
    "agent.json",
];

/// The exact top-level entries a kind directory must contain — no more, no less.
/// Two files plus the two metadata subdirectories; anything else fails the build.
const KIND_ROOT_ENTRIES: &[&str] = &["agent.json", "instructions.md", "capabilities", "skills"];

/// The exact top-level entries the shared `common/` base must contain: the two
/// metadata subdirectories plus the shared `instructions.md` preamble prepended
/// to every kind. No `agent.json` (it is not a kind).
const COMMON_ROOT_ENTRIES: &[&str] = &["capabilities", "skills", "instructions.md"];

/// The sole file the `capabilities/` subdirectory may contain.
const CAPABILITIES_DIR_ENTRIES: &[&str] = &["capabilities.json"];

/// The sole file the `skills/` subdirectory may contain.
const SKILLS_DIR_ENTRIES: &[&str] = &["skills.json"];

/// The shared `common/` base inherited by every kind: default capability/skill
/// names plus the instructions preamble prepended to each kind's prompt.
pub struct CommonBase {
    pub capabilities: Vec<String>,
    pub skills: Vec<String>,
    /// Absolute path to `common/instructions.md`, the shared preamble.
    pub instructions_path: PathBuf,
}

/// Parse the shared `common/` base at `common_dir`.
///
/// `common/` carries the default `capabilities/`, `skills/`, and the
/// `instructions.md` preamble inherited by every kind. All three are required
/// (like a kind's), but it must **not** declare an agent kind: an `agent.json`
/// there is an error.
///
/// # Errors
///
/// Errors if `common/` contains `agent.json`, has a missing/stray entry, or if
/// its `capabilities.json` / `skills.json` is malformed.
pub fn parse_common(common_dir: &Path) -> Result<CommonBase> {
    if common_dir.join("agent.json").is_file() {
        bail!(
            "{} must not contain agent.json: the shared base defines default \
             capabilities/skills/instructions inherited by all kinds, not an agent kind",
            common_dir.display()
        );
    }

    // The shared base layout is fixed: the two metadata subdirectories (each
    // holding exactly its one JSON file) plus the instructions preamble — no
    // more, no less.
    ensure_exact_entries(common_dir, COMMON_ROOT_ENTRIES)?;
    ensure_exact_entries(&common_dir.join("capabilities"), CAPABILITIES_DIR_ENTRIES)?;
    ensure_exact_entries(&common_dir.join("skills"), SKILLS_DIR_ENTRIES)?;

    let capabilities: CapabilitiesJson = read_json(common_dir, "capabilities/capabilities.json")?;
    let skills: SkillsJson = read_json(common_dir, "skills/skills.json")?;

    let instructions_path = common_dir.join("instructions.md");
    if !instructions_path.is_file() {
        bail!(
            "{} must be a file, not a directory",
            instructions_path.display()
        );
    }

    Ok(CommonBase {
        capabilities: capabilities.capabilities,
        skills: skills.skills,
        instructions_path,
    })
}

/// Parse and validate the kind directory at `dir`.
///
/// The directory name is the source of truth for the kind: `agent.json`'s
/// declared `kind` must match it, otherwise the build fails.
pub fn parse_kind(dir: &Path) -> Result<ParsedManifest> {
    let dir_name = dir
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(|| anyhow!("agent kind directory has no UTF-8 name: {}", dir.display()))?
        .to_string();

    // A kind directory's layout is fixed: exactly the two files plus the two
    // metadata subdirectories, each holding exactly its one JSON file. A missing
    // *or* stray entry fails the build before any content is parsed.
    ensure_exact_entries(dir, KIND_ROOT_ENTRIES)?;
    ensure_exact_entries(&dir.join("capabilities"), CAPABILITIES_DIR_ENTRIES)?;
    ensure_exact_entries(&dir.join("skills"), SKILLS_DIR_ENTRIES)?;

    let agent: AgentJson = read_json(dir, "agent.json")?;
    if agent.kind != dir_name {
        bail!(
            "kind mismatch in {}: directory is '{dir_name}' but agent.json declares '{}'",
            dir.display(),
            agent.kind
        );
    }

    let capabilities: CapabilitiesJson = read_json(dir, "capabilities/capabilities.json")?;
    let skills: SkillsJson = read_json(dir, "skills/skills.json")?;

    let instructions_path = dir.join("instructions.md");
    if !instructions_path.is_file() {
        bail!(
            "{} must be a file, not a directory",
            instructions_path.display()
        );
    }

    Ok(ParsedManifest {
        kind: agent.kind,
        description: agent.description,
        spawn_enabled: agent.spawn.enabled,
        allowed_kinds: agent.spawn.allowed_kinds,
        retries: agent.runtime.retries,
        tool_block_retries: agent.runtime.tool_block_retries,
        capabilities: capabilities.capabilities,
        skills: skills.skills,
        instructions_path,
        // Filled in by `inherit_base` once the common base is parsed.
        common_instructions_path: None,
    })
}

/// Enforce that `dir` contains **exactly** `expected` — no missing entry and no
/// stray one — so the baked manifest layout is fully fixed at compile time.
///
/// Entry names are compared regardless of file/dir kind; the callers separately
/// validate kind where it matters (e.g. `instructions.md` must be a file).
/// Hidden entries (names starting with `.`, e.g. `.gitkeep` / `.DS_Store`) are
/// ignored so VCS/OS artifacts do not fail the build, matching the rest of the
/// generator's treatment of hidden paths.
///
/// # Errors
///
/// Errors if `dir` cannot be read, a required entry is missing, or an
/// unexpected entry is present.
pub(crate) fn ensure_exact_entries(dir: &Path, expected: &[&str]) -> Result<()> {
    let mut actual: Vec<String> = Vec::new();
    for entry in fs::read_dir(dir).with_context(|| format!("reading {}", dir.display()))? {
        let entry = entry?;
        let name = entry
            .file_name()
            .to_str()
            .ok_or_else(|| anyhow!("non-UTF-8 entry name in {}", dir.display()))?
            .to_string();
        if name.starts_with('.') {
            continue;
        }
        actual.push(name);
    }

    for want in expected {
        if !actual.iter().any(|name| name == want) {
            bail!(
                "{}: missing required entry '{want}' (the manifest layout is fixed: \
                 exactly {expected:?})",
                dir.display()
            );
        }
    }
    for got in &actual {
        if !expected.iter().any(|want| want == got) {
            bail!(
                "{}: unexpected entry '{got}' (the manifest layout is fixed: \
                 exactly {expected:?}; remove it or add it to the schema)",
                dir.display()
            );
        }
    }
    Ok(())
}

/// Read and deserialize `dir/<relative>` as JSON, wrapping IO/parse errors with
/// the offending path.
fn read_json<T: serde::de::DeserializeOwned>(dir: &Path, relative: &str) -> Result<T> {
    let path = dir.join(relative);
    let text = fs::read_to_string(&path).with_context(|| format!("reading {}", path.display()))?;
    serde_json::from_str(&text).with_context(|| format!("parsing {}", path.display()))
}
