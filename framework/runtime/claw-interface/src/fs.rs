//! The `ClawFs` filesystem injection trait.
//!
//! This is the persistence seam for everything that has to survive a reboot
//! (conversation tapes, profile/long-term memory, …). Like [`ClawHttp`], it is a
//! dependency-injection point: the espidf wiring implements it over the DATA
//! root (FATFS / SD card), while host tests provide `std::fs` or an in-memory
//! map. Modules never touch `std::fs` directly so they stay portable.
//!
//! # Two layers: a filesystem that produces file handles
//!
//! The seam is shaped like a (statically dispatched) `vfs`:
//! - [`ClawFs`] is the *filesystem* — it locates paths and produces handles
//!   ([`open`](ClawFs::open) / [`create`](ClawFs::create) /
//!   [`open_append`](ClawFs::open_append)) and performs whole-path operations
//!   that have no handle (`rename`, `remove`, `list_dir`, …).
//! - [`ClawFile`] is an *open handle*: read/seek/write against one file without
//!   reopening it. Callers that touch the same file repeatedly (e.g. loading a
//!   conversation log record-by-record) hold one handle and seek within it,
//!   instead of reopening the file per access.
//!
//! For the common one-shot cases, [`ClawFs`] provides path-addressed
//! conveniences ([`read`](ClawFs::read), [`read_at`](ClawFs::read_at),
//! [`append`](ClawFs::append), [`write_atomic`](ClawFs::write_atomic), …) as
//! default methods implemented over the handle primitives, so callers that do
//! not need a persistent handle keep a terse API.
//!
//! Paths are byte-oriented, opaque strings already resolved against the DATA
//! root by the caller (`claw_paths`); this trait does no path joining.
//!
//! [`ClawHttp`]: crate::http::ClawHttp

/// Filesystem failure.
///
/// Deliberately coarse: callers either retry, log, or fall back to an empty
/// state, so the only distinction that matters is "the file isn't there" versus
/// "the underlying I/O failed". The `esp_err_t` mapping for the C ABI lives in
/// `claw_capi`.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum FsError {
    #[error("path not found")]
    NotFound,
    #[error("filesystem io error: {0}")]
    Io(String),
}

/// An open file handle produced by a [`ClawFs`].
///
/// One handle addresses one file without reopening it: the read cursor advances
/// across [`read_to_end`](ClawFile::read_to_end), and [`read_exact_at`] seeks to
/// an absolute offset. This is the primitive the append-only journals rely on to
/// fetch records by indexed `(offset, len)` while holding the file open once.
///
/// [`read_exact_at`]: ClawFile::read_exact_at
pub trait ClawFile {
    /// Read from the current cursor to end of file.
    fn read_to_end(&mut self) -> Result<Vec<u8>, FsError>;

    /// Seek to absolute byte `offset` and read exactly `len` bytes.
    ///
    /// Used to fetch a single record out of an append-only log via its indexed
    /// `(offset, len)`. An `offset`/`len` past the end of the file is an
    /// [`FsError::Io`] (a corrupt/short file), not a silent truncation:
    /// implementations return exactly `len` bytes on success.
    fn read_exact_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, FsError>;

    /// Byte length of the underlying file.
    fn size(&self) -> Result<u64, FsError>;

    /// Write all of `data` at the handle's current write position.
    fn write_all(&mut self, data: &[u8]) -> Result<(), FsError>;
}

/// Byte-oriented persistence injection point: a filesystem that hands out
/// [`ClawFile`] handles.
///
/// Implementations must be safe to share across threads: the same filesystem
/// backend can be held by stores, skill registries, and tool/capability paths
/// driven from different runtime boundaries. File handles remain per-operation;
/// sharing the filesystem object must not imply sharing one open handle.
///
/// Two write disciplines coexist:
/// - [`open_append`](ClawFs::open) + [`read_exact_at`](ClawFile::read_exact_at)
///   for append-only journals (e.g. the conversation data `.jsonl`), where each
///   turn appends a record and load reads back only the live records by byte
///   offset.
/// - [`write_atomic`](ClawFs::write_atomic) for whole-file checkpoints that must
///   replace the target tear-free: the small index manifest (`.json`) rewritten
///   on compaction/collapse, and the `.jsonl` itself when a collapse rewrites it
///   to drop dead records. The default implementation writes a temporary sibling
///   then [`rename`](ClawFs::rename)s it over the target.
pub trait ClawFs: Send + Sync {
    /// The open-file handle this filesystem produces.
    type File: ClawFile;

    /// Open an existing file for reading. Returns [`FsError::NotFound`] when
    /// `path` is absent.
    fn open(&self, path: &str) -> Result<Self::File, FsError>;

    /// Create (or truncate) `path` for writing, creating parent directories as
    /// needed. The returned handle starts empty at offset 0.
    fn create(&self, path: &str) -> Result<Self::File, FsError>;

    /// Open `path` for appending, creating it (and parents) if absent.
    ///
    /// Writes through the returned handle go after whatever is already there, so
    /// prior records are never rewritten. A crash mid-append may leave a torn
    /// trailing record, which readers discard; it must never corrupt earlier
    /// records.
    fn open_append(&self, path: &str) -> Result<Self::File, FsError>;

    /// Rename `from` to `to`, replacing `to` if it exists. Returns
    /// [`FsError::NotFound`] when `from` is absent.
    ///
    /// This is the atomic-replace primitive behind
    /// [`write_atomic`](Self::write_atomic): a crash mid-rename leaves either the
    /// old target or the new one, never a torn mix.
    fn rename(&self, from: &str, to: &str) -> Result<(), FsError>;

    /// Recursively create directory `path`, including any missing parents.
    ///
    /// Idempotent: succeeds if the directory already exists. Backends with no
    /// explicit directory concept (e.g. a flat key→bytes map) treat this as a
    /// no-op — their directories exist implicitly the moment a file is written
    /// beneath them, and [`list_dir`](ClawFs::list_dir) on such a path already
    /// reports an empty listing rather than [`FsError::NotFound`].
    fn create_dir_all(&self, path: &str) -> Result<(), FsError>;

    /// Whether `path` currently exists.
    fn exists(&self, path: &str) -> bool;

    /// Remove `path`. Removing a missing path succeeds (idempotent).
    fn remove(&self, path: &str) -> Result<(), FsError>;

    /// List the immediate entry names within directory `path`.
    ///
    /// Returns only the final path component of each entry (e.g. `"light_switch"`),
    /// not joined paths, and in unspecified order — callers that need ordering
    /// sort themselves. Both files and subdirectories are included. Returns
    /// [`FsError::NotFound`] when `path` does not exist.
    fn list_dir(&self, path: &str) -> Result<Vec<String>, FsError>;

    // ----------------------------------------------------------------------
    // Path-addressed conveniences (default methods over the handle primitives).
    //
    // These cover the one-shot cases where a caller does not need to hold a
    // handle. Implementations may override them where a path-level shortcut is
    // cheaper than open+operate (e.g. `len` via `stat`, `write_atomic` with
    // pretty-printing).
    // ----------------------------------------------------------------------

    /// Read the whole file. Returns [`FsError::NotFound`] when `path` is absent.
    fn read(&self, path: &str) -> Result<Vec<u8>, FsError> {
        self.open(path)?.read_to_end()
    }

    /// Read `len` bytes starting at byte `offset` (a one-shot
    /// [`open`](Self::open) + [`read_exact_at`](ClawFile::read_exact_at)).
    fn read_at(&self, path: &str, offset: u64, len: usize) -> Result<Vec<u8>, FsError> {
        self.open(path)?.read_exact_at(offset, len)
    }

    /// Byte length of `path`. Returns [`FsError::NotFound`] when absent.
    fn len(&self, path: &str) -> Result<u64, FsError> {
        self.open(path)?.size()
    }

    /// Append `data` to the end of `path`, creating it if absent (a one-shot
    /// [`open_append`](Self::open_append) + [`write_all`](ClawFile::write_all)).
    fn append(&self, path: &str, data: &[u8]) -> Result<(), FsError> {
        self.open_append(path)?.write_all(data)
    }

    /// Durably replace `path` with `data`.
    ///
    /// The default writes to a temporary `"{path}.tmp"` sibling and
    /// [`rename`](Self::rename)s it over the target so a crash mid-write never
    /// leaves a half-written file — the file is either the old contents or the
    /// new contents, never a torn mix.
    fn write_atomic(&self, path: &str, data: &[u8]) -> Result<(), FsError> {
        let tmp = format!("{path}.tmp");
        {
            let mut file = self.create(&tmp)?;
            file.write_all(data)?;
        }
        self.rename(&tmp, path)
    }
}

// ===========================================================================
// Reference implementations (feature-gated)
// ===========================================================================
//
// These are host-only `ClawFs` backends, kept beside the trait so the handful
// of distinct implementations live in exactly one place. They are NOT part of
// the platform-free seam the rest of this crate provides, so each is gated
// behind its own opt-in feature and must never be enabled in a device build:
//
// - `memfs`: an in-memory map used as a hermetic test double.
// - `diskfs`: a `std::fs` backend used by the host CLIs and disk-backed tests.

#[cfg(feature = "memfs")]
mod memfs {
    use std::collections::{BTreeSet, HashMap};
    use std::sync::{Arc, Mutex, MutexGuard};

    use super::{ClawFile, ClawFs, FsError};

    /// In-memory [`ClawFs`] backed by a path → bytes map.
    ///
    /// A cheap, `Clone`able handle over a shared store, mirroring how [`DiskFs`]
    /// is a handle over the on-disk filesystem: cloning a `MemFs` does not copy
    /// the contents, it yields another handle to the *same* store. The sharing
    /// (the `Arc`) is an internal detail, so callers pass `MemFs` by value to a
    /// generic `F: ClawFs` bound and never wrap it in an `Arc` themselves.
    ///
    /// Hermetic and thread-safe, so host tests can exercise persistence through
    /// cloned store/registry handles without touching the real filesystem.
    /// `list_dir` derives entries from the key prefixes, mirroring a real
    /// directory tree.
    ///
    /// [`DiskFs`]: super::DiskFs
    #[derive(Debug, Clone, Default)]
    pub struct MemFs {
        files: Arc<Mutex<HashMap<String, Vec<u8>>>>,
    }

    impl MemFs {
        /// An empty filesystem.
        pub fn new() -> Self {
            Self::default()
        }

        fn lock(&self) -> MutexGuard<'_, HashMap<String, Vec<u8>>> {
            self.files
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
        }
    }

    /// An open handle into a [`MemFs`] store.
    ///
    /// Holds the shared store plus the key it addresses; reads slice the live
    /// bytes under the lock, writes extend them. Writes go to the store
    /// immediately (there is no separate "flush"), matching the on-disk backend
    /// closely enough for tests.
    pub struct MemFile {
        files: Arc<Mutex<HashMap<String, Vec<u8>>>>,
        path: String,
    }

    impl MemFile {
        fn lock(&self) -> MutexGuard<'_, HashMap<String, Vec<u8>>> {
            self.files
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
        }
    }

    impl ClawFile for MemFile {
        fn read_to_end(&mut self) -> Result<Vec<u8>, FsError> {
            self.lock()
                .get(&self.path)
                .cloned()
                .ok_or(FsError::NotFound)
        }

        fn read_exact_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, FsError> {
            let files = self.lock();
            let bytes = files.get(&self.path).ok_or(FsError::NotFound)?;
            let start =
                usize::try_from(offset).map_err(|_| FsError::Io("offset overflow".into()))?;
            let end = start
                .checked_add(len)
                .filter(|end| *end <= bytes.len())
                .ok_or_else(|| FsError::Io("read_at past end of file".into()))?;
            bytes
                .get(start..end)
                .map(<[u8]>::to_vec)
                .ok_or_else(|| FsError::Io("read_at past end of file".into()))
        }

        fn size(&self) -> Result<u64, FsError> {
            self.lock()
                .get(&self.path)
                .map(|bytes| u64::try_from(bytes.len()).unwrap_or(u64::MAX))
                .ok_or(FsError::NotFound)
        }

        fn write_all(&mut self, data: &[u8]) -> Result<(), FsError> {
            self.lock()
                .entry(self.path.clone())
                .or_default()
                .extend_from_slice(data);
            Ok(())
        }
    }

    impl ClawFs for MemFs {
        type File = MemFile;

        fn open(&self, path: &str) -> Result<Self::File, FsError> {
            if self.lock().contains_key(path) {
                Ok(MemFile {
                    files: Arc::clone(&self.files),
                    path: path.to_string(),
                })
            } else {
                Err(FsError::NotFound)
            }
        }

        fn create(&self, path: &str) -> Result<Self::File, FsError> {
            // Truncate: an empty entry that subsequent `write_all`s extend.
            self.lock().insert(path.to_string(), Vec::new());
            Ok(MemFile {
                files: Arc::clone(&self.files),
                path: path.to_string(),
            })
        }

        fn open_append(&self, path: &str) -> Result<Self::File, FsError> {
            self.lock().entry(path.to_string()).or_default();
            Ok(MemFile {
                files: Arc::clone(&self.files),
                path: path.to_string(),
            })
        }

        fn rename(&self, from: &str, to: &str) -> Result<(), FsError> {
            let mut files = self.lock();
            let bytes = files.remove(from).ok_or(FsError::NotFound)?;
            files.insert(to.to_string(), bytes);
            Ok(())
        }

        fn create_dir_all(&self, _path: &str) -> Result<(), FsError> {
            // Flat key→bytes map: directories are implicit in key prefixes, so
            // there is nothing to materialize. `list_dir` of an empty prefix
            // already returns an empty listing.
            Ok(())
        }

        fn exists(&self, path: &str) -> bool {
            self.lock().contains_key(path)
        }

        fn remove(&self, path: &str) -> Result<(), FsError> {
            self.lock().remove(path);
            Ok(())
        }

        fn list_dir(&self, path: &str) -> Result<Vec<String>, FsError> {
            let prefix = format!("{}/", path.trim_end_matches('/'));
            let mut names = BTreeSet::new();
            for key in self.lock().keys() {
                if let Some(rest) = key.strip_prefix(&prefix) {
                    if let Some(name) = rest.split('/').next().filter(|name| !name.is_empty()) {
                        names.insert(name.to_string());
                    }
                }
            }
            Ok(names.into_iter().collect())
        }
    }
}

#[cfg(feature = "diskfs")]
mod diskfs {
    use std::io::{Read, Seek, SeekFrom, Write};
    use std::path::PathBuf;

    use super::{ClawFile, ClawFs, FsError};

    fn map_io(error: std::io::Error) -> FsError {
        if error.kind() == std::io::ErrorKind::NotFound {
            FsError::NotFound
        } else {
            FsError::Io(error.to_string())
        }
    }

    /// Host [`ClawFs`] over `std::fs`.
    ///
    /// Two addressing modes share one durable write discipline (write to a `.tmp`
    /// sibling then `rename`, creating parent directories as needed):
    /// - [`absolute`](DiskFs::absolute): paths are used verbatim. Used by the
    ///   host CLIs and conversation-memory tests that already hold absolute paths.
    /// - [`rooted`](DiskFs::rooted): paths are joined onto a base directory (a
    ///   leading `/` is stripped so absolute-looking virtual paths stay inside the
    ///   root), keeping on-disk fixtures portable. Used by the skill-registry
    ///   tests.
    #[derive(Debug, Clone, Default)]
    pub struct DiskFs {
        base: Option<PathBuf>,
        #[cfg(feature = "diskfs-pretty")]
        pretty_json: bool,
    }

    impl DiskFs {
        /// Verbatim-path mode: the trait `path` is the on-disk path.
        pub fn absolute() -> Self {
            Self::default()
        }

        /// Rooted mode: the trait `path` is joined onto `base` (leading `/`
        /// stripped) so virtual paths resolve inside the root.
        pub fn rooted(base: impl Into<PathBuf>) -> Self {
            Self {
                base: Some(base.into()),
                #[cfg(feature = "diskfs-pretty")]
                pretty_json: false,
            }
        }

        /// Pretty-print `.json` writes so the on-disk files are readable when
        /// inspecting a test's output directory. Off by default.
        #[cfg(feature = "diskfs-pretty")]
        pub fn with_pretty_json(mut self, enabled: bool) -> Self {
            self.pretty_json = enabled;
            self
        }

        fn resolve(&self, path: &str) -> PathBuf {
            match &self.base {
                Some(base) => base.join(path.trim_start_matches('/')),
                None => PathBuf::from(path),
            }
        }

        /// Ensure the parent directory of `full` exists before a write.
        fn ensure_parent(full: &std::path::Path) -> Result<(), FsError> {
            if let Some(parent) = full.parent() {
                std::fs::create_dir_all(parent).map_err(|error| FsError::Io(error.to_string()))?;
            }
            Ok(())
        }
    }

    /// An open handle over a [`std::fs::File`].
    pub struct DiskFile {
        file: std::fs::File,
    }

    impl ClawFile for DiskFile {
        fn read_to_end(&mut self) -> Result<Vec<u8>, FsError> {
            let mut buffer = Vec::new();
            self.file
                .read_to_end(&mut buffer)
                .map_err(|error| FsError::Io(error.to_string()))?;
            Ok(buffer)
        }

        fn read_exact_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, FsError> {
            self.file
                .seek(SeekFrom::Start(offset))
                .map_err(|error| FsError::Io(error.to_string()))?;
            let mut buffer = vec![0u8; len];
            self.file
                .read_exact(&mut buffer)
                .map_err(|error| FsError::Io(error.to_string()))?;
            Ok(buffer)
        }

        fn size(&self) -> Result<u64, FsError> {
            self.file
                .metadata()
                .map(|metadata| metadata.len())
                .map_err(map_io)
        }

        fn write_all(&mut self, data: &[u8]) -> Result<(), FsError> {
            self.file
                .write_all(data)
                .map_err(|error| FsError::Io(error.to_string()))
        }
    }

    impl ClawFs for DiskFs {
        type File = DiskFile;

        fn open(&self, path: &str) -> Result<Self::File, FsError> {
            std::fs::File::open(self.resolve(path))
                .map(|file| DiskFile { file })
                .map_err(map_io)
        }

        fn create(&self, path: &str) -> Result<Self::File, FsError> {
            let full = self.resolve(path);
            Self::ensure_parent(&full)?;
            std::fs::File::create(&full)
                .map(|file| DiskFile { file })
                .map_err(|error| FsError::Io(error.to_string()))
        }

        fn open_append(&self, path: &str) -> Result<Self::File, FsError> {
            let full = self.resolve(path);
            Self::ensure_parent(&full)?;
            std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&full)
                .map(|file| DiskFile { file })
                .map_err(|error| FsError::Io(error.to_string()))
        }

        fn rename(&self, from: &str, to: &str) -> Result<(), FsError> {
            std::fs::rename(self.resolve(from), self.resolve(to)).map_err(map_io)
        }

        fn create_dir_all(&self, path: &str) -> Result<(), FsError> {
            std::fs::create_dir_all(self.resolve(path))
                .map_err(|error| FsError::Io(error.to_string()))
        }

        fn exists(&self, path: &str) -> bool {
            self.resolve(path).exists()
        }

        fn remove(&self, path: &str) -> Result<(), FsError> {
            match std::fs::remove_file(self.resolve(path)) {
                Ok(()) => Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => Err(FsError::Io(error.to_string())),
            }
        }

        fn list_dir(&self, path: &str) -> Result<Vec<String>, FsError> {
            let entries = std::fs::read_dir(self.resolve(path)).map_err(map_io)?;
            let mut names = Vec::new();
            for entry in entries {
                let entry = entry.map_err(|error| FsError::Io(error.to_string()))?;
                if let Some(name) = entry.file_name().to_str() {
                    names.push(name.to_string());
                }
            }
            Ok(names)
        }

        /// Byte length via `stat`, avoiding an open just to read metadata.
        fn len(&self, path: &str) -> Result<u64, FsError> {
            std::fs::metadata(self.resolve(path))
                .map(|metadata| metadata.len())
                .map_err(map_io)
        }

        /// Durable whole-file replace: write a `.tmp` sibling, then `rename` it
        /// over the target. Overrides the trait default to add optional
        /// pretty-printing of `.json` payloads (the `diskfs-pretty` feature).
        fn write_atomic(&self, path: &str, data: &[u8]) -> Result<(), FsError> {
            let full = self.resolve(path);
            Self::ensure_parent(&full)?;
            #[cfg(feature = "diskfs-pretty")]
            let payload = self.render(path, data);
            #[cfg(not(feature = "diskfs-pretty"))]
            let payload = std::borrow::Cow::Borrowed(data);
            let mut tmp = full.clone().into_os_string();
            tmp.push(".tmp");
            let tmp = PathBuf::from(tmp);
            std::fs::write(&tmp, payload.as_ref())
                .map_err(|error| FsError::Io(error.to_string()))?;
            std::fs::rename(&tmp, &full).map_err(|error| FsError::Io(error.to_string()))
        }
    }

    #[cfg(feature = "diskfs-pretty")]
    impl DiskFs {
        /// Pretty-print `.json` payloads when enabled; otherwise pass through.
        fn render<'data>(&self, path: &str, data: &'data [u8]) -> std::borrow::Cow<'data, [u8]> {
            if self.pretty_json && path.ends_with(".json") {
                serde_json::from_slice::<serde_json::Value>(data)
                    .ok()
                    .and_then(|value| serde_json::to_vec_pretty(&value).ok())
                    .map(std::borrow::Cow::Owned)
                    .unwrap_or(std::borrow::Cow::Borrowed(data))
            } else {
                std::borrow::Cow::Borrowed(data)
            }
        }
    }
}

#[cfg(feature = "memfs")]
pub use memfs::{MemFile, MemFs};

#[cfg(feature = "diskfs")]
pub use diskfs::{DiskFile, DiskFs};
