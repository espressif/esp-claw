//! The `SandboxFs` filesystem API.
//!
//! This is the file-access surface the agent sees from inside the sandbox. It
//! mirrors the CRUD shape of [`ClawFs`] but speaks only in **virtual paths**
//! rooted at the visible sandbox roots (see [`VISIBLE_PREFIXES`] and
//! `docs/sandbox.md`), and surfaces sandbox-specific failures — a path outside
//! the sandbox, or a write to a read-only root — as distinct error variants
//! rather than folding them into a generic I/O error.
//!
//! [`ClawFs`]: claw_interface::ClawFs

use claw_interface::FsError;

/// The virtual path prefixes that are visible inside the sandbox.
///
/// A virtual path is accepted only if, after normalization, it equals one of
/// these prefixes (minus the trailing slash) or lies beneath it. Every other
/// path — including the bare `/shared/` and `/system/` roots, which are *not*
/// listed here — is rejected with [`SandboxError::OutsideSandbox`].
///
/// See `docs/sandbox.md` for the lifetime and visibility semantics of each root.
pub const VISIBLE_PREFIXES: &[&str] = &[
    "/sandbox/",
    "/shared/skills/",
    "/shared/tmp/",
    "/shared/data/",
    "/system/skills/",
];

/// The visible prefixes the sandbox may read but must never modify.
///
/// A write, append, or remove targeting one of these (even though it is
/// otherwise visible) is rejected with [`SandboxError::ReadOnly`].
pub const READ_ONLY_PREFIXES: &[&str] = &["/system/"];

/// A sandbox filesystem failure.
///
/// Distinguishes the sandbox's own access decisions ([`OutsideSandbox`],
/// [`ReadOnly`]) from a failure of the backing store, which is wrapped verbatim
/// as [`Fs`]. Keeping these separate lets callers tell "you asked for something
/// you are not allowed to touch" apart from "the storage layer failed".
///
/// [`OutsideSandbox`]: SandboxError::OutsideSandbox
/// [`ReadOnly`]: SandboxError::ReadOnly
/// [`Fs`]: SandboxError::Fs
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum SandboxError {
    /// The path does not begin with a visible sandbox root, or it tried to
    /// escape its root (e.g. via `..`). The string is the offending virtual
    /// path as supplied by the caller.
    #[error("path is outside the sandbox: {0}")]
    OutsideSandbox(String),

    /// A mutating operation targeted a read-only root (see
    /// [`READ_ONLY_PREFIXES`]). The string is the offending virtual path.
    #[error("path is read-only: {0}")]
    ReadOnly(String),

    /// The backing filesystem failed.
    #[error("filesystem error: {0}")]
    Fs(#[from] FsError),
}

/// The sandboxed file-access API.
///
/// All paths are **virtual** paths inside the sandbox (e.g. `/sandbox/tmp/x`,
/// `/shared/data/y`); implementations validate each path against
/// [`VISIBLE_PREFIXES`] and route it to the appropriate backing store. Methods
/// mirror [`ClawFs`], with two deliberate differences:
///
/// - every method can fail with [`SandboxError::OutsideSandbox`] when the path
///   is not addressable from within the sandbox;
/// - mutating methods can fail with [`SandboxError::ReadOnly`] on a read-only
///   root.
///
/// Implementations must be safe to share across threads (handed out via `Arc`),
/// matching the threading contract of the underlying [`ClawFs`].
///
/// [`ClawFs`]: claw_interface::ClawFs
pub trait SandboxFs: Send + Sync {
    /// Read the whole file at `path`.
    fn read(&self, path: &str) -> Result<Vec<u8>, SandboxError>;

    /// Read `len` bytes starting at byte `offset`.
    fn read_at(&self, path: &str, offset: u64, len: usize) -> Result<Vec<u8>, SandboxError>;

    /// Byte length of `path`.
    fn len(&self, path: &str) -> Result<u64, SandboxError>;

    /// Durably replace `path` with `data`.
    fn write_atomic(&self, path: &str, data: &[u8]) -> Result<(), SandboxError>;

    /// Append `data` to the end of `path`, creating it if absent.
    fn append(&self, path: &str, data: &[u8]) -> Result<(), SandboxError>;

    /// Whether `path` currently exists.
    ///
    /// Returns `Ok(false)` for a valid-but-absent path and
    /// [`SandboxError::OutsideSandbox`] for a path the sandbox cannot address —
    /// the two are distinct, unlike [`ClawFs::exists`], which collapses both
    /// into `false`.
    ///
    /// [`ClawFs::exists`]: claw_interface::ClawFs::exists
    fn exists(&self, path: &str) -> Result<bool, SandboxError>;

    /// Remove `path`. Removing a missing (but visible) path succeeds.
    fn remove(&self, path: &str) -> Result<(), SandboxError>;

    /// List the immediate entry names within directory `path`.
    fn list_dir(&self, path: &str) -> Result<Vec<String>, SandboxError>;
}
