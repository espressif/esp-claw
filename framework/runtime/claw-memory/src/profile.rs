//! Editable profile documents: global, durable assistant/user context.
//!
//! Profile documents are not long-term facts. They are small, whole-file
//! documents edited by users or by profile-specific tools and later projected into
//! context by `claw-core`.

use std::marker::PhantomData;

use claw_interface::{ClawFs, FsError};
use strum::{EnumString, IntoStaticStr};

/// Filename for the assistant soul/persona document.
pub const SOUL_FILE: &str = "soul.md";
/// Filename for the assistant identity card document.
pub const ASSISTANT_IDENTITY_FILE: &str = "identity.md";
/// Filename for the default user's profile document.
pub const USER_PROFILE_FILE: &str = "user.md";

/// Default maximum size of one profile document.
pub const DEFAULT_PROFILE_DOCUMENT_MAX_BYTES: usize = 8192;

/// One editable global profile document.
#[derive(Clone, Copy, Debug, EnumString, IntoStaticStr, PartialEq, Eq, Hash)]
#[strum(
    ascii_case_insensitive,
    parse_err_ty = ParseProfileDocumentError,
    parse_err_fn = ParseProfileDocumentError::new
)]
pub enum ProfileDocument {
    /// Assistant behavior principles, persona, and style.
    #[strum(serialize = "soul")]
    Soul,
    /// Assistant/device name, role, capabilities, and boundaries.
    #[strum(to_string = "assistant_identity", serialize = "identity")]
    AssistantIdentity,
    /// The single user's stable preferences and interaction agreements.
    #[strum(to_string = "user_profile", serialize = "user")]
    UserProfile,
}

impl ProfileDocument {
    /// Stable document id used in tools and diagnostics.
    pub fn id(self) -> &'static str {
        self.into()
    }

    /// On-disk filename under the profile store directory.
    pub fn file_name(self) -> &'static str {
        match self {
            ProfileDocument::Soul => SOUL_FILE,
            ProfileDocument::AssistantIdentity => ASSISTANT_IDENTITY_FILE,
            ProfileDocument::UserProfile => USER_PROFILE_FILE,
        }
    }

    /// The three canonical profile documents in context order.
    pub fn all() -> [Self; 3] {
        [
            ProfileDocument::Soul,
            ProfileDocument::AssistantIdentity,
            ProfileDocument::UserProfile,
        ]
    }
}

impl std::fmt::Display for ProfileDocument {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str((*self).into())
    }
}

/// Failure parsing a profile document id.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("unknown profile document '{value}'")]
pub struct ParseProfileDocumentError {
    value: String,
}

impl ParseProfileDocumentError {
    fn new(value: &str) -> Self {
        Self {
            value: value.to_string(),
        }
    }
}

/// Failure from a profile document operation.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ProfileError {
    /// The underlying filesystem operation failed.
    #[error("profile document {document} filesystem error: {source}")]
    File {
        /// Document being accessed.
        document: ProfileDocument,
        /// Filesystem failure.
        #[source]
        source: FsError,
    },
    /// The document is larger than the configured cap.
    #[error("profile document {document} is too large: {actual_bytes} bytes exceeds {max_bytes}")]
    TooLarge {
        /// Document being accessed.
        document: ProfileDocument,
        /// Configured maximum bytes.
        max_bytes: usize,
        /// Actual bytes read or written.
        actual_bytes: usize,
    },
    /// The document is not UTF-8 text.
    #[error("profile document {document} is not valid utf-8")]
    InvalidUtf8 {
        /// Document being accessed.
        document: ProfileDocument,
    },
}

/// Current contents of all profile documents.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ProfileSnapshot {
    /// Contents of `soul.md`, or `None` when the file is absent.
    pub soul: Option<String>,
    /// Contents of `identity.md`, or `None` when the file is absent.
    pub assistant_identity: Option<String>,
    /// Contents of `user.md`, or `None` when the file is absent.
    pub user_profile: Option<String>,
}

/// Pure storage for the editable profile documents.
pub struct ProfileStore<F: ClawFs + 'static> {
    /// Directory holding `soul.md`, `identity.md`, and `user.md`.
    dir: String,
    /// Maximum accepted byte length for each profile document.
    max_document_bytes: usize,
    _fs: PhantomData<fn() -> F>,
}

impl<F: ClawFs + 'static> Clone for ProfileStore<F> {
    fn clone(&self) -> Self {
        Self {
            dir: self.dir.clone(),
            max_document_bytes: self.max_document_bytes,
            _fs: PhantomData,
        }
    }
}

impl<F: ClawFs + 'static> ProfileStore<F> {
    /// Build a store rooted at `dir` over the selected filesystem backend.
    pub fn new(dir: &str) -> Self {
        Self {
            dir: dir.to_string(),
            max_document_bytes: DEFAULT_PROFILE_DOCUMENT_MAX_BYTES,
            _fs: PhantomData,
        }
    }

    /// The configured profile directory.
    pub fn dir(&self) -> &str {
        &self.dir
    }

    /// Full path to a document.
    pub fn path(&self, document: ProfileDocument) -> String {
        join_path(&self.dir, document.file_name())
    }

    /// Read one document. Missing files are normal absence, not an error.
    pub fn read(&self, document: ProfileDocument) -> Result<Option<String>, ProfileError> {
        let path = self.path(document);
        let bytes = match F::read(&path) {
            Ok(bytes) => bytes,
            Err(FsError::NotFound) => return Ok(None),
            Err(source) => return Err(ProfileError::File { document, source }),
        };
        self.decode(document, bytes).map(Some)
    }

    /// Read all canonical profile documents.
    pub fn snapshot(&self) -> Result<ProfileSnapshot, ProfileError> {
        Ok(ProfileSnapshot {
            soul: self.read(ProfileDocument::Soul)?,
            assistant_identity: self.read(ProfileDocument::AssistantIdentity)?,
            user_profile: self.read(ProfileDocument::UserProfile)?,
        })
    }

    /// Atomically replace one document with `content`.
    pub fn replace(
        &self,
        document: ProfileDocument,
        content: impl AsRef<str>,
    ) -> Result<(), ProfileError> {
        let bytes = content.as_ref().as_bytes();
        self.check_size(document, bytes.len())?;
        let path = self.path(document);
        F::write_atomic(&path, bytes).map_err(|source| ProfileError::File { document, source })
    }

    /// Create a document with `content` only when it does not already exist.
    ///
    /// Returns `true` when the document was created.
    pub fn ensure_default(
        &self,
        document: ProfileDocument,
        content: impl AsRef<str>,
    ) -> Result<bool, ProfileError> {
        if F::exists(&self.path(document)) {
            return Ok(false);
        }
        self.replace(document, content)?;
        Ok(true)
    }

    /// Atomically clear one document. The file remains present but contributes no
    /// context because empty content is semantically absent.
    pub fn clear(&self, document: ProfileDocument) -> Result<(), ProfileError> {
        self.replace(document, "")
    }

    fn decode(&self, document: ProfileDocument, bytes: Vec<u8>) -> Result<String, ProfileError> {
        self.check_size(document, bytes.len())?;
        String::from_utf8(bytes).map_err(|_| ProfileError::InvalidUtf8 { document })
    }

    fn check_size(
        &self,
        document: ProfileDocument,
        actual_bytes: usize,
    ) -> Result<(), ProfileError> {
        if actual_bytes > self.max_document_bytes {
            return Err(ProfileError::TooLarge {
                document,
                max_bytes: self.max_document_bytes,
                actual_bytes,
            });
        }
        Ok(())
    }
}

fn join_path(dir: &str, file_name: &str) -> String {
    let dir = dir.trim_end_matches('/');
    if dir.is_empty() {
        file_name.to_string()
    } else {
        format!("{dir}/{file_name}")
    }
}
