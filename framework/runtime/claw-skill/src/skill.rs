//! Skill identity, catalog metadata, and `SKILL.md` front-matter parsing.

use std::borrow::Cow;
use std::fmt;

use claw_interface::FsError;
use serde::Deserialize;
use strum::{EnumString, IntoStaticStr};
use thiserror::Error;

/// A skill's identity: its directory name under a skills root.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct SkillId(Cow<'static, str>);

impl SkillId {
    /// Wrap a runtime directory name as a skill id.
    pub fn new(id: impl Into<String>) -> Self {
        Self(Cow::Owned(id.into()))
    }

    /// Wrap a static id without allocation.
    pub const fn from_static(id: &'static str) -> Self {
        Self(Cow::Borrowed(id))
    }

    /// The id as a string slice.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for SkillId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

/// Runtime interpretation of `metadata.manage_mode`.
#[derive(Clone, Copy, Debug, Default, EnumString, IntoStaticStr, PartialEq, Eq)]
#[strum(
    parse_err_ty = ParseSkillManageModeError,
    parse_err_fn = ParseSkillManageModeError::new
)]
pub enum SkillManageMode {
    /// `"readonly"` and `"web"` are both treated as read-only on device.
    #[default]
    #[strum(to_string = "readonly", serialize = "web")]
    Readonly,
    /// `"runtime"` skills may be owned by runtime installers.
    #[strum(serialize = "runtime")]
    Runtime,
}

/// Failure parsing a skill management mode.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
#[error("unknown skill manage mode; expected readonly, web, or runtime")]
pub struct ParseSkillManageModeError;

impl ParseSkillManageModeError {
    fn new(_: &str) -> Self {
        Self
    }
}

/// Metadata nested under the `metadata` key in `SKILL.md` front-matter.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SkillFrontmatterMetadata {
    cap_groups: Vec<String>,
    manage_mode: SkillManageMode,
    category: Vec<String>,
    peripherals: Vec<String>,
    tags: Vec<String>,
}

impl SkillFrontmatterMetadata {
    /// Capability groups declared by this skill. Parsed but not wired to tool
    /// visibility in this Rust implementation pass.
    pub fn cap_groups(&self) -> &[String] {
        &self.cap_groups
    }

    /// Skill management mode after device-side normalization.
    pub fn manage_mode(&self) -> SkillManageMode {
        self.manage_mode
    }

    /// Optional category labels from Skills Lab metadata.
    pub fn category(&self) -> &[String] {
        &self.category
    }

    /// Optional peripheral labels from Skills Lab metadata.
    pub fn peripherals(&self) -> &[String] {
        &self.peripherals
    }

    /// Optional search tags from Skills Lab metadata.
    pub fn tags(&self) -> &[String] {
        &self.tags
    }
}

/// One catalog entry plus the document source needed for activation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Skill {
    id: SkillId,
    name: String,
    description: String,
    author: Option<String>,
    metadata: SkillFrontmatterMetadata,
    file: String,
    pub(crate) root: String,
}

impl Skill {
    /// The skill id, equal to the containing directory name.
    pub fn id(&self) -> &SkillId {
        &self.id
    }

    /// The `name` declared in front-matter.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// One-line description shown in the catalog.
    pub fn description(&self) -> &str {
        &self.description
    }

    /// Optional front-matter author.
    pub fn author(&self) -> Option<&str> {
        self.author.as_deref()
    }

    /// Relative document path, normally `<id>/SKILL.md`.
    pub fn file(&self) -> &str {
        &self.file
    }

    /// Parsed front-matter metadata.
    pub fn metadata(&self) -> &SkillFrontmatterMetadata {
        &self.metadata
    }
}

/// An activated skill document snapshot.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SkillDocument {
    content: String,
}

impl SkillDocument {
    pub(crate) fn new(content: String) -> Self {
        Self { content }
    }

    /// Processed document content returned by `skill_activate`.
    pub fn content(&self) -> &str {
        &self.content
    }

    /// Consume the snapshot and return the owned content.
    pub fn into_content(self) -> String {
        self.content
    }
}

/// Failure reading, parsing, or resolving a skill.
#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum SkillError {
    /// No skill with the given id is registered.
    #[error("skill not found: {0}")]
    NotFound(SkillId),
    /// Listing a skills root directory failed.
    #[error("failed to scan skills root '{0}': {1}")]
    ScanFailed(String, FsError),
    /// Reading a skill's `SKILL.md` failed.
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
    /// A skill's front-matter block is not valid JSON.
    #[error("skill '{0}' has invalid front-matter JSON: {1}")]
    InvalidJson(SkillId, String),
    /// A skill's front-matter is valid JSON but violates the skill contract.
    #[error("skill '{0}' has invalid front-matter: {1}")]
    InvalidFrontmatter(SkillId, String),
}

#[derive(Deserialize)]
struct RawFrontMatter {
    name: Option<String>,
    description: Option<String>,
    author: Option<String>,
    metadata: Option<RawMetadata>,
}

#[derive(Default, Deserialize)]
struct RawMetadata {
    #[serde(default)]
    cap_groups: Vec<String>,
    manage_mode: Option<String>,
    #[serde(default)]
    category: Vec<String>,
    #[serde(default)]
    peripherals: Vec<String>,
    #[serde(default)]
    tags: Vec<String>,
}

pub(crate) fn parse_front_matter(id: SkillId, root: &str, head: &str) -> Result<Skill, SkillError> {
    let (json, _) = front_matter_sections(&id, head)?;
    let front_matter: RawFrontMatter = serde_json::from_str(json.trim())
        .map_err(|error| SkillError::InvalidJson(id.clone(), error.to_string()))?;

    let name = required_string(&id, "name", front_matter.name)?;
    if name != id.as_str() {
        return Err(SkillError::InvalidFrontmatter(
            id,
            format!("front-matter name '{name}' must match the skill directory name"),
        ));
    }
    let description = required_string(&id, "description", front_matter.description)?;
    let raw_metadata = front_matter
        .metadata
        .ok_or_else(|| SkillError::InvalidFrontmatter(id.clone(), "missing metadata".into()))?;
    let metadata = parse_metadata(&id, raw_metadata)?;
    let file = format!("{}/SKILL.md", id.as_str());

    Ok(Skill {
        id,
        name,
        description,
        author: front_matter.author,
        metadata,
        file,
        root: root.to_owned(),
    })
}

pub(crate) fn front_matter_sections<'a>(
    id: &SkillId,
    text: &'a str,
) -> Result<(&'a str, &'a str), SkillError> {
    let after_open = text
        .trim_start()
        .strip_prefix("---")
        .ok_or_else(|| SkillError::MissingOpeningFence(id.clone()))?;
    let close = after_open
        .find("\n---")
        .ok_or_else(|| SkillError::MissingClosingFence(id.clone()))?;
    let body = if let Some(closing_fence) = after_open[close..].strip_prefix('\n') {
        if let Some((_, body)) = closing_fence.split_once('\n') {
            body
        } else {
            ""
        }
    } else {
        ""
    };
    Ok((&after_open[..close], body))
}

fn required_string(
    id: &SkillId,
    field: &'static str,
    value: Option<String>,
) -> Result<String, SkillError> {
    let value = value
        .ok_or_else(|| SkillError::InvalidFrontmatter(id.clone(), format!("missing {field}")))?;
    if value.trim().is_empty() {
        return Err(SkillError::InvalidFrontmatter(
            id.clone(),
            format!("{field} must not be empty"),
        ));
    }
    Ok(value)
}

fn parse_metadata(id: &SkillId, raw: RawMetadata) -> Result<SkillFrontmatterMetadata, SkillError> {
    let manage_mode = required_string(id, "metadata.manage_mode", raw.manage_mode)?;
    let manage_mode = SkillManageMode::try_from(manage_mode.as_str()).map_err(|_| {
        SkillError::InvalidFrontmatter(
            id.clone(),
            format!("unsupported metadata.manage_mode '{manage_mode}'"),
        )
    })?;

    Ok(SkillFrontmatterMetadata {
        cap_groups: raw.cap_groups,
        manage_mode,
        category: raw.category,
        peripherals: raw.peripherals,
        tags: raw.tags,
    })
}
