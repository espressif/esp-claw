//! Editable profile documents: global, durable assistant/user context.
//!
//! Profile documents are not long-term facts. They are small, whole-file
//! documents edited by users or by profile-specific tools and later projected into
//! context by `claw-core`.

use core::str::FromStr;

use std::marker::PhantomData;

use claw_interface::{ClawFs, FsError};

/// Filename for the assistant soul/persona document.
pub const SOUL_FILE: &str = "soul.md";
/// Filename for the assistant identity card document.
pub const ASSISTANT_IDENTITY_FILE: &str = "identity.md";
/// Filename for the default user's profile document.
pub const USER_PROFILE_FILE: &str = "user.md";

/// Default maximum size of one profile document.
pub const DEFAULT_PROFILE_DOCUMENT_MAX_BYTES: usize = 8192;

/// One editable global profile document.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ProfileDocument {
    /// Assistant behavior principles, persona, and style.
    Soul,
    /// Assistant/device name, role, capabilities, and boundaries.
    AssistantIdentity,
    /// The single user's stable preferences and interaction agreements.
    UserProfile,
}

impl ProfileDocument {
    /// Stable document id used in tools and diagnostics.
    pub fn id(self) -> &'static str {
        match self {
            ProfileDocument::Soul => "soul",
            ProfileDocument::AssistantIdentity => "assistant_identity",
            ProfileDocument::UserProfile => "user_profile",
        }
    }

    /// On-disk filename under [`ProfileConfig::dir`].
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
        formatter.write_str(self.id())
    }
}

impl FromStr for ProfileDocument {
    type Err = ParseProfileDocumentError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        let normalized = value.trim();
        if normalized.eq_ignore_ascii_case("soul") {
            return Ok(ProfileDocument::Soul);
        }
        if normalized.eq_ignore_ascii_case("identity")
            || normalized.eq_ignore_ascii_case("assistant_identity")
        {
            return Ok(ProfileDocument::AssistantIdentity);
        }
        if normalized.eq_ignore_ascii_case("user")
            || normalized.eq_ignore_ascii_case("user_profile")
        {
            return Ok(ProfileDocument::UserProfile);
        }
        Err(ParseProfileDocumentError {
            value: normalized.to_string(),
        })
    }
}

/// Failure parsing a profile document id.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("unknown profile document '{value}'")]
pub struct ParseProfileDocumentError {
    value: String,
}

/// Tuning for [`ProfileStore`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProfileConfig {
    /// Directory holding `soul.md`, `identity.md`, and `user.md`.
    pub dir: String,
    /// Maximum accepted byte length for each profile document.
    pub max_document_bytes: usize,
}

impl ProfileConfig {
    /// Build a profile config rooted at `dir`.
    pub fn new(dir: &str) -> Self {
        Self {
            dir: dir.to_string(),
            max_document_bytes: DEFAULT_PROFILE_DOCUMENT_MAX_BYTES,
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
    config: ProfileConfig,
    _fs: PhantomData<fn() -> F>,
}

impl<F: ClawFs + 'static> Clone for ProfileStore<F> {
    fn clone(&self) -> Self {
        Self {
            config: self.config.clone(),
            _fs: PhantomData,
        }
    }
}

impl<F: ClawFs + 'static> ProfileStore<F> {
    /// Build a store over `config` and the selected filesystem backend.
    pub fn new(config: ProfileConfig) -> Self {
        Self {
            config,
            _fs: PhantomData,
        }
    }

    /// The configured profile directory.
    pub fn dir(&self) -> &str {
        &self.config.dir
    }

    /// Full path to a document.
    pub fn path(&self, document: ProfileDocument) -> String {
        join_path(&self.config.dir, document.file_name())
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
        if actual_bytes > self.config.max_document_bytes {
            return Err(ProfileError::TooLarge {
                document,
                max_bytes: self.config.max_document_bytes,
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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use claw_interface::MemFs;

    fn store() -> ProfileStore<MemFs> {
        MemFs::new();
        ProfileStore::new(ProfileConfig::new("/memory"))
    }

    #[test]
    fn missing_document_is_absent() {
        let store = store();
        assert_eq!(store.read(ProfileDocument::Soul).unwrap(), None);
    }

    #[test]
    fn replace_and_read_round_trip() {
        let store = store();
        store.replace(ProfileDocument::Soul, "Be concise.").unwrap();
        assert_eq!(
            store.read(ProfileDocument::Soul).unwrap(),
            Some("Be concise.".to_string())
        );
    }

    #[test]
    fn clear_keeps_file_but_returns_empty_content() {
        let store = store();
        store
            .replace(ProfileDocument::UserProfile, "Use Chinese.")
            .unwrap();
        store.clear(ProfileDocument::UserProfile).unwrap();
        assert_eq!(
            store.read(ProfileDocument::UserProfile).unwrap(),
            Some(String::new())
        );
    }

    #[test]
    fn rejects_too_large_document() {
        MemFs::new();
        let store = ProfileStore::<MemFs>::new(ProfileConfig {
            max_document_bytes: 2,
            ..ProfileConfig::new("/memory")
        });
        let error = store
            .replace(ProfileDocument::AssistantIdentity, "abc")
            .unwrap_err();
        assert!(matches!(error, ProfileError::TooLarge { .. }));
    }

    #[test]
    fn invalid_utf8_is_an_error() {
        let store = store();
        MemFs::write_atomic("/memory/soul.md", &[0xff]).unwrap();
        let error = store.read(ProfileDocument::Soul).unwrap_err();
        assert!(matches!(error, ProfileError::InvalidUtf8 { .. }));
    }

    #[test]
    fn parses_document_ids() {
        assert_eq!("soul".parse(), Ok(ProfileDocument::Soul));
        assert_eq!("identity".parse(), Ok(ProfileDocument::AssistantIdentity));
        assert_eq!("user_profile".parse(), Ok(ProfileDocument::UserProfile));
    }
}
