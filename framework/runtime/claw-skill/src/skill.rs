//! Skill identity, catalog metadata, and `SKILL.md` front-matter parsing.
//!
//! A skill lives in `<root>/<id>/SKILL.md`, whose head is a JSON front-matter
//! block fenced by `---` lines:
//!
//! ```text
//! ---
//! { "name": "...", "description": "..." }
//! ---
//! # markdown body …
//! ```
//!
//! Only `description` is read into the catalog; the id comes from the skill
//! directory name. Any other front-matter keys (tooling/Skills-Lab fields) are
//! ignored. The catalog lives entirely in that head, so it is read without
//! touching the potentially large body.

use std::borrow::Cow;

use claw_interface::FsError;
use serde::Deserialize;
use thiserror::Error;

/// A skill's identity — its directory name under the skills root.
///
/// Backed by `Cow<'static, str>` so a compile-time-baked id (from a generated
/// agent manifest) borrows a `&'static str` with no allocation, while a runtime
/// id owns its `String`. Both compare equal by content, so a baked id matches a
/// runtime-loaded one.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SkillId(Cow<'static, str>);

impl SkillId {
    /// Wrap a runtime directory name as a skill id (owns its `String`).
    pub fn new(id: impl Into<String>) -> Self {
        Self(Cow::Owned(id.into()))
    }

    /// Wrap a `&'static str` as a skill id in a `const` context (no allocation) —
    /// used by build-script-generated manifests.
    pub const fn from_static(id: &'static str) -> Self {
        Self(Cow::Borrowed(id))
    }

    /// The id as a string slice.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Display for SkillId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

/// Cheap catalog row for one skill — everything except the document body.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SkillMetadata {
    id: SkillId,
    description: String,
}

impl SkillMetadata {
    /// The skill's identity.
    pub fn id(&self) -> &SkillId {
        &self.id
    }

    /// One-line summary shown in the skills catalog.
    pub fn description(&self) -> &str {
        &self.description
    }
}

/// Failure reading, parsing, or resolving a skill.
///
/// Each variant pins down a specific failure so a caller can tell a missing
/// directory from malformed front-matter from a JSON error — and the underlying
/// [`FsError`] is wrapped, not flattened into a string.
#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum SkillError {
    /// Listing a skills root directory failed (root, cause).
    #[error("failed to scan skills root '{0}': {1}")]
    ScanFailed(String, FsError),
    /// Reading a skill's `SKILL.md` failed (skill id, cause).
    #[error("failed to read skill '{0}': {1}")]
    ReadFailed(SkillId, FsError),
    /// A skill's `SKILL.md` bytes were not valid UTF-8.
    #[error("skill '{0}' is not valid UTF-8")]
    InvalidUtf8(SkillId),
    /// A skill's front-matter is missing its opening `---` fence.
    #[error("skill '{0}' is missing the opening '---' front-matter fence")]
    MissingOpeningFence(SkillId),
    /// A skill's front-matter is missing its closing `---` fence.
    #[error("skill '{0}' is missing the closing '---' front-matter fence")]
    MissingClosingFence(SkillId),
    /// A skill's front-matter block is not valid JSON (skill id, parser message).
    #[error("skill '{0}' has invalid front-matter JSON: {1}")]
    InvalidJson(SkillId, String),
    /// No skill with the given id is registered.
    #[error("skill not found: {0}")]
    NotFound(SkillId),
}

/// The JSON shape of a `SKILL.md` front-matter block. Unknown keys are ignored.
#[derive(Deserialize)]
struct FrontMatter {
    #[serde(default)]
    description: String,
}

/// Parse the front-matter block at the head of a `SKILL.md` into a [`SkillMetadata`].
///
/// `head` must begin with the file's first bytes (a bounded prefix is enough —
/// the metadata lives between the first two `---` fences). The `id` comes from
/// the skill directory name, not the file.
///
/// # Errors
///
/// - [`SkillError::MissingOpeningFence`] / [`SkillError::MissingClosingFence`] if
///   the `---` fences are absent.
/// - [`SkillError::InvalidJson`] if the fenced block is not valid JSON.
pub(crate) fn parse_front_matter(id: SkillId, head: &str) -> Result<SkillMetadata, SkillError> {
    let json = front_matter_json(&id, head)?;
    let front_matter: FrontMatter = serde_json::from_str(json.trim())
        .map_err(|error| SkillError::InvalidJson(id.clone(), error.to_string()))?;
    Ok(SkillMetadata {
        id,
        description: front_matter.description,
    })
}

/// The JSON text between the opening and closing `---` fences.
fn front_matter_json<'a>(id: &SkillId, text: &'a str) -> Result<&'a str, SkillError> {
    let after_open = text
        .trim_start()
        .strip_prefix("---")
        .ok_or_else(|| SkillError::MissingOpeningFence(id.clone()))?;
    let close = after_open
        .find("\n---")
        .ok_or_else(|| SkillError::MissingClosingFence(id.clone()))?;
    Ok(after_open.get(..close).unwrap_or(""))
}

/// Return the markdown body of a `SKILL.md` — everything after the closing fence.
///
/// # Errors
///
/// [`SkillError::MissingOpeningFence`] / [`SkillError::MissingClosingFence`] if
/// the `---` fences are absent.
pub(crate) fn strip_front_matter<'a>(id: &SkillId, text: &'a str) -> Result<&'a str, SkillError> {
    let after_open = text
        .trim_start()
        .strip_prefix("---")
        .ok_or_else(|| SkillError::MissingOpeningFence(id.clone()))?;
    let close = after_open
        .find("\n---")
        .ok_or_else(|| SkillError::MissingClosingFence(id.clone()))?;
    // From the closing fence, skip the rest of that fence line, then the body.
    let from_fence = after_open.get(close..).unwrap_or("");
    let body = from_fence
        .get(1..) // drop the leading '\n' so the fence line starts the slice
        .and_then(|line| {
            line.find('\n')
                .and_then(|newline_index| line.get(newline_index.saturating_add(1)..))
        })
        .unwrap_or("");
    Ok(body)
}

// Parser error/edge paths only — degenerate inputs, no skill fixtures. Real
// `SKILL.md` parsing is covered in `tests/registry.rs` over an in-memory fs.
#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    #[test]
    fn missing_opening_fence_errors() {
        let error = parse_front_matter(SkillId::new("x"), "no front matter").unwrap_err();
        assert!(matches!(error, SkillError::MissingOpeningFence(_)));
    }

    #[test]
    fn missing_close_fence_errors() {
        let error = parse_front_matter(SkillId::new("x"), "---\n{}\n").unwrap_err();
        assert!(matches!(error, SkillError::MissingClosingFence(_)));
    }

    #[test]
    fn invalid_json_errors() {
        let error = parse_front_matter(SkillId::new("x"), "---\nnot json\n---\nbody").unwrap_err();
        assert!(matches!(error, SkillError::InvalidJson(_, _)));
    }

    #[test]
    fn parses_description_and_ignores_unknown_keys() {
        let metadata = parse_front_matter(
            SkillId::new("x"),
            "---\n{\"description\":\"d\",\"metadata\":{\"cap_groups\":[\"x\"]}}\n---\nbody",
        )
        .unwrap();
        assert_eq!(metadata.description(), "d");
        assert_eq!(metadata.id().as_str(), "x");
    }
}
