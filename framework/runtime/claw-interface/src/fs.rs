//! The `ClawFs` filesystem injection trait.
//!
//! This is the persistence seam for everything that has to survive a reboot
//! (conversation tapes, profile/long-term memory, …). Like [`ClawHttp`], it is a
//! dependency-injection point: the espidf wiring implements it over the DATA
//! root (FATFS / SD card), while host tests provide `std::fs` or an in-memory
//! map. Modules never touch `std::fs` directly so they stay portable.
//!
//! # Two layers: a filesystem backend that produces file handles
//!
//! The seam is shaped like a statically dispatched `std::fs` HAL:
//! - [`ClawFs`] is the platform filesystem backend selected by type. It locates
//!   paths and produces handles ([`open`](ClawFs::open) /
//!   [`create`](ClawFs::create) / [`open_append`](ClawFs::open_append)) and
//!   performs whole-path operations that have no handle (`rename`, `remove`,
//!   `list_dir`, …).
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

use std::error::Error;
use std::fmt;
use std::sync::Arc;

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
    Io(#[from] FsIoError),
}

impl FsError {
    /// Preserve a concrete filesystem-adjacent failure.
    pub fn io(error: impl Error + Send + Sync + 'static) -> Self {
        Self::Io(FsIoError::new(error))
    }

    /// Build an I/O error for synthetic filesystem failures that have no lower
    /// source error.
    pub fn io_message(message: impl Into<String>) -> Self {
        Self::Io(FsIoError::message(message))
    }
}

impl From<std::io::Error> for FsError {
    fn from(error: std::io::Error) -> Self {
        if error.kind() == std::io::ErrorKind::NotFound {
            Self::NotFound
        } else {
            Self::io(error)
        }
    }
}

/// Source-preserving filesystem I/O failure.
#[derive(Debug, Clone)]
pub struct FsIoError {
    source: Arc<dyn Error + Send + Sync + 'static>,
}

impl FsIoError {
    /// Preserve a concrete underlying error.
    pub fn new(error: impl Error + Send + Sync + 'static) -> Self {
        Self {
            source: Arc::new(error),
        }
    }

    /// Create a synthetic source for filesystem failures raised by validation
    /// logic rather than a lower I/O API.
    pub fn message(message: impl Into<String>) -> Self {
        Self::new(FsMessageError(message.into()))
    }
}

impl fmt::Display for FsIoError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.source.fmt(formatter)
    }
}

impl Error for FsIoError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        Some(self.source.as_ref())
    }
}

impl PartialEq for FsIoError {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.source, &other.source)
            || match (
                self.source.as_ref().downcast_ref::<FsMessageError>(),
                other.source.as_ref().downcast_ref::<FsMessageError>(),
            ) {
                (Some(left), Some(right)) => left == right,
                _ => false,
            }
    }
}

impl Eq for FsIoError {}

#[derive(Debug, PartialEq, Eq, thiserror::Error)]
#[error("{0}")]
struct FsMessageError(String);

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

/// Byte-oriented persistence injection point: a filesystem backend that hands out
/// [`ClawFile`] handles.
///
/// This is a `std::fs`-like HAL: operations are associated functions on the
/// selected backend type rather than methods on a runtime-owned filesystem
/// handle. Per-platform state such as mount tables, DATA roots, or test fixtures
/// lives behind the implementation.
///
/// Two write disciplines coexist:
/// - [`open_append`](ClawFs::open_append) +
///   [`read_exact_at`](ClawFile::read_exact_at) for append-only journals (e.g.
///   the conversation data `.jsonl`), where each turn appends a record and load
///   reads back only the live records by byte offset.
/// - [`write_atomic`](ClawFs::write_atomic) for whole-file checkpoints that must
///   replace the target tear-free: the small index manifest (`.json`) rewritten
///   on compaction/collapse, and the `.jsonl` itself when a collapse rewrites it
///   to drop dead records. The default implementation writes a temporary sibling
///   then [`rename`](ClawFs::rename)s it over the target.
pub trait ClawFs: Send + Sync + 'static {
    /// The open-file handle this filesystem produces.
    type File: ClawFile;

    /// Open an existing file for reading. Returns [`FsError::NotFound`] when
    /// `path` is absent.
    fn open(path: &str) -> Result<Self::File, FsError>;

    /// Create (or truncate) `path` for writing, creating parent directories as
    /// needed. The returned handle starts empty at offset 0.
    fn create(path: &str) -> Result<Self::File, FsError>;

    /// Open `path` for appending, creating it (and parents) if absent.
    ///
    /// Writes through the returned handle go after whatever is already there, so
    /// prior records are never rewritten. A crash mid-append may leave a torn
    /// trailing record, which readers discard; it must never corrupt earlier
    /// records.
    fn open_append(path: &str) -> Result<Self::File, FsError>;

    /// Rename `from` to `to`, replacing `to` if it exists. Returns
    /// [`FsError::NotFound`] when `from` is absent.
    ///
    /// This is the atomic-replace primitive behind
    /// [`write_atomic`](Self::write_atomic): a crash mid-rename leaves either the
    /// old target or the new one, never a torn mix.
    fn rename(from: &str, to: &str) -> Result<(), FsError>;

    /// Recursively create directory `path`, including any missing parents.
    ///
    /// Idempotent: succeeds if the directory already exists. Backends with no
    /// explicit directory concept (e.g. a flat key→bytes map) treat this as a
    /// no-op — their directories exist implicitly the moment a file is written
    /// beneath them, and [`list_dir`](ClawFs::list_dir) on such a path already
    /// reports an empty listing rather than [`FsError::NotFound`].
    fn create_dir_all(path: &str) -> Result<(), FsError>;

    /// Whether `path` currently exists.
    fn exists(path: &str) -> bool;

    /// Remove a file or empty directory at `path`. Removing a missing path
    /// succeeds (idempotent).
    fn remove(path: &str) -> Result<(), FsError>;

    /// List the immediate entry names within directory `path`.
    ///
    /// Returns only the final path component of each entry (e.g. `"light_switch"`),
    /// not joined paths, and in unspecified order — callers that need ordering
    /// sort themselves. Both files and subdirectories are included. Returns
    /// [`FsError::NotFound`] when `path` does not exist.
    fn list_dir(path: &str) -> Result<Vec<String>, FsError>;

    // ----------------------------------------------------------------------
    // Path-addressed conveniences (default methods over the handle primitives).
    //
    // These cover the one-shot cases where a caller does not need to hold a
    // handle. Implementations may override them where a path-level shortcut is
    // cheaper than open+operate (e.g. `len` via `stat`, `write_atomic` with
    // pretty-printing).
    // ----------------------------------------------------------------------

    /// Read the whole file. Returns [`FsError::NotFound`] when `path` is absent.
    fn read(path: &str) -> Result<Vec<u8>, FsError> {
        Self::open(path)?.read_to_end()
    }

    /// Read `len` bytes starting at byte `offset` (a one-shot
    /// [`open`](Self::open) + [`read_exact_at`](ClawFile::read_exact_at)).
    fn read_at(path: &str, offset: u64, len: usize) -> Result<Vec<u8>, FsError> {
        Self::open(path)?.read_exact_at(offset, len)
    }

    /// Byte length of `path`. Returns [`FsError::NotFound`] when absent.
    fn len(path: &str) -> Result<u64, FsError> {
        Self::open(path)?.size()
    }

    /// Append `data` to the end of `path`, creating it if absent (a one-shot
    /// [`open_append`](Self::open_append) + [`write_all`](ClawFile::write_all)).
    fn append(path: &str, data: &[u8]) -> Result<(), FsError> {
        Self::open_append(path)?.write_all(data)
    }

    /// Durably replace `path` with `data`.
    ///
    /// The default writes to a temporary `"{path}.tmp"` sibling and
    /// [`rename`](Self::rename)s it over the target so a crash mid-write never
    /// leaves a half-written file — the file is either the old contents or the
    /// new contents, never a torn mix.
    fn write_atomic(path: &str, data: &[u8]) -> Result<(), FsError> {
        let tmp = format!("{path}.tmp");
        {
            let mut file = Self::create(&tmp)?;
            file.write_all(data)?;
        }
        Self::rename(&tmp, path)
    }
}

// ===========================================================================
// Reference implementations (feature-gated)
// ===========================================================================
//
// These are host-target `ClawFs` backends, kept beside the trait so the handful
// of distinct implementations live in exactly one place. They are NOT part of
// the platform-free seam the rest of this crate provides, so each is gated
// behind its own opt-in feature and must never be enabled in a device build:
//
// - `memfs`: an in-memory map used as a hermetic test double.
// - `diskfs`: a `std::fs` backend used by the host CLIs and disk-backed tests.

#[cfg(feature = "memfs")]
mod memfs {
    use std::cell::RefCell;
    use std::collections::{BTreeSet, HashMap};
    use std::sync::{Arc, Mutex, MutexGuard};

    use super::{ClawFile, ClawFs, FsError};

    type Files = Arc<Mutex<HashMap<String, Vec<u8>>>>;

    thread_local! {
        static FILES: RefCell<Files> = RefCell::new(Arc::new(Mutex::new(HashMap::new())));
    }

    /// In-memory [`ClawFs`] backed by a thread-local path → bytes map.
    ///
    /// `MemFs` is a backend type, not a storage handle. [`MemFs::new`] resets the
    /// current thread's test fixture and returns the zero-sized selector. File
    /// handles keep an `Arc` to the fixture they were opened against, so already
    /// opened handles remain valid even if a later test reset installs a fresh
    /// fixture.
    ///
    /// Hermetic per test thread, so host tests can exercise persistence without
    /// touching the real filesystem.
    /// `list_dir` derives entries from the key prefixes, mirroring a real
    /// directory tree.
    ///
    /// [`DiskFs`]: super::DiskFs
    #[derive(Debug, Clone, Copy)]
    pub struct MemFs;

    impl Default for MemFs {
        fn default() -> Self {
            Self::new()
        }
    }

    impl MemFs {
        /// An empty filesystem.
        pub fn new() -> Self {
            FILES.with(|slot| *slot.borrow_mut() = Arc::new(Mutex::new(HashMap::new())));
            Self
        }

        /// Clear the current thread's in-memory filesystem fixture.
        pub fn clear() {
            FILES.with(|slot| {
                slot.borrow()
                    .lock()
                    .unwrap_or_else(|poisoned| poisoned.into_inner())
                    .clear();
            });
        }

        fn files() -> Files {
            FILES.with(|slot| Arc::clone(&slot.borrow()))
        }

        fn lock(files: &Files) -> MutexGuard<'_, HashMap<String, Vec<u8>>> {
            files
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
            MemFs::lock(&self.files)
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
                usize::try_from(offset).map_err(|_| FsError::io_message("offset overflow"))?;
            let end = start
                .checked_add(len)
                .filter(|end| *end <= bytes.len())
                .ok_or_else(|| FsError::io_message("read_at past end of file"))?;
            bytes
                .get(start..end)
                .map(<[u8]>::to_vec)
                .ok_or_else(|| FsError::io_message("read_at past end of file"))
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

        fn open(path: &str) -> Result<Self::File, FsError> {
            let files = Self::files();
            if Self::lock(&files).contains_key(path) {
                Ok(MemFile {
                    files,
                    path: path.to_string(),
                })
            } else {
                Err(FsError::NotFound)
            }
        }

        fn create(path: &str) -> Result<Self::File, FsError> {
            let files = Self::files();
            // Truncate: an empty entry that subsequent `write_all`s extend.
            Self::lock(&files).insert(path.to_string(), Vec::new());
            Ok(MemFile {
                files,
                path: path.to_string(),
            })
        }

        fn open_append(path: &str) -> Result<Self::File, FsError> {
            let files = Self::files();
            Self::lock(&files).entry(path.to_string()).or_default();
            Ok(MemFile {
                files,
                path: path.to_string(),
            })
        }

        fn rename(from: &str, to: &str) -> Result<(), FsError> {
            let files = Self::files();
            let mut files = Self::lock(&files);
            let bytes = files.remove(from).ok_or(FsError::NotFound)?;
            files.insert(to.to_string(), bytes);
            Ok(())
        }

        fn create_dir_all(_path: &str) -> Result<(), FsError> {
            // Flat key→bytes map: directories are implicit in key prefixes, so
            // there is nothing to materialize. `list_dir` of an empty prefix
            // already returns an empty listing.
            Ok(())
        }

        fn exists(path: &str) -> bool {
            let files = Self::files();
            let exists = Self::lock(&files).contains_key(path);
            exists
        }

        fn remove(path: &str) -> Result<(), FsError> {
            let files = Self::files();
            Self::lock(&files).remove(path);
            Ok(())
        }

        fn list_dir(path: &str) -> Result<Vec<String>, FsError> {
            let prefix = format!("{}/", path.trim_end_matches('/'));
            let mut names = BTreeSet::new();
            let files = Self::files();
            for key in Self::lock(&files).keys() {
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
    use std::cell::RefCell;
    use std::io::{Read, Seek, SeekFrom, Write};
    use std::path::PathBuf;

    use super::{ClawFile, ClawFs, FsError};

    fn map_io(error: std::io::Error) -> FsError {
        FsError::from(error)
    }

    #[derive(Debug, Clone, Default)]
    struct DiskConfig {
        base: Option<PathBuf>,
        #[cfg(feature = "diskfs-pretty")]
        pretty_json: bool,
    }

    thread_local! {
        static CONFIG: RefCell<DiskConfig> = RefCell::new(DiskConfig::default());
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
    #[derive(Debug, Clone, Copy, Default)]
    pub struct DiskFs;

    impl DiskFs {
        /// Verbatim-path mode: the trait `path` is the on-disk path.
        pub fn absolute() -> Self {
            CONFIG.with(|slot| *slot.borrow_mut() = DiskConfig::default());
            Self
        }

        /// Rooted mode: the trait `path` is joined onto `base` (leading `/`
        /// stripped) so virtual paths resolve inside the root.
        pub fn rooted(base: impl Into<PathBuf>) -> Self {
            CONFIG.with(|slot| {
                *slot.borrow_mut() = DiskConfig {
                    base: Some(base.into()),
                    #[cfg(feature = "diskfs-pretty")]
                    pretty_json: false,
                };
            });
            Self
        }

        /// Pretty-print `.json` writes so the on-disk files are readable when
        /// inspecting a test's output directory. Off by default.
        #[cfg(feature = "diskfs-pretty")]
        pub fn with_pretty_json(self, enabled: bool) -> Self {
            CONFIG.with(|slot| slot.borrow_mut().pretty_json = enabled);
            self
        }

        fn resolve(path: &str) -> PathBuf {
            CONFIG.with(|slot| match &slot.borrow().base {
                Some(base) => base.join(path.trim_start_matches('/')),
                None => PathBuf::from(path),
            })
        }

        /// Ensure the parent directory of `full` exists before a write.
        fn ensure_parent(full: &std::path::Path) -> Result<(), FsError> {
            if let Some(parent) = full.parent() {
                std::fs::create_dir_all(parent).map_err(FsError::from)?;
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
            self.file.read_to_end(&mut buffer).map_err(FsError::from)?;
            Ok(buffer)
        }

        fn read_exact_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>, FsError> {
            self.file
                .seek(SeekFrom::Start(offset))
                .map_err(FsError::from)?;
            let mut buffer = vec![0u8; len];
            self.file.read_exact(&mut buffer).map_err(FsError::from)?;
            Ok(buffer)
        }

        fn size(&self) -> Result<u64, FsError> {
            self.file
                .metadata()
                .map(|metadata| metadata.len())
                .map_err(map_io)
        }

        fn write_all(&mut self, data: &[u8]) -> Result<(), FsError> {
            self.file.write_all(data).map_err(FsError::from)
        }
    }

    impl ClawFs for DiskFs {
        type File = DiskFile;

        fn open(path: &str) -> Result<Self::File, FsError> {
            std::fs::File::open(Self::resolve(path))
                .map(|file| DiskFile { file })
                .map_err(map_io)
        }

        fn create(path: &str) -> Result<Self::File, FsError> {
            let full = Self::resolve(path);
            Self::ensure_parent(&full)?;
            std::fs::File::create(&full)
                .map(|file| DiskFile { file })
                .map_err(FsError::from)
        }

        fn open_append(path: &str) -> Result<Self::File, FsError> {
            let full = Self::resolve(path);
            Self::ensure_parent(&full)?;
            std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(&full)
                .map(|file| DiskFile { file })
                .map_err(FsError::from)
        }

        fn rename(from: &str, to: &str) -> Result<(), FsError> {
            std::fs::rename(Self::resolve(from), Self::resolve(to)).map_err(map_io)
        }

        fn create_dir_all(path: &str) -> Result<(), FsError> {
            std::fs::create_dir_all(Self::resolve(path)).map_err(FsError::from)
        }

        fn exists(path: &str) -> bool {
            Self::resolve(path).exists()
        }

        fn remove(path: &str) -> Result<(), FsError> {
            let full = Self::resolve(path);
            let result = match std::fs::symlink_metadata(&full) {
                Ok(metadata) if metadata.is_dir() => std::fs::remove_dir(full),
                Ok(_) => std::fs::remove_file(full),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(()),
                Err(error) => return Err(FsError::from(error)),
            };
            match result {
                Ok(()) => Ok(()),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
                Err(error) => Err(FsError::from(error)),
            }
        }

        fn list_dir(path: &str) -> Result<Vec<String>, FsError> {
            let entries = std::fs::read_dir(Self::resolve(path)).map_err(map_io)?;
            let mut names = Vec::new();
            for entry in entries {
                let entry = entry.map_err(FsError::from)?;
                if let Some(name) = entry.file_name().to_str() {
                    names.push(name.to_string());
                }
            }
            Ok(names)
        }

        /// Byte length via `stat`, avoiding an open just to read metadata.
        fn len(path: &str) -> Result<u64, FsError> {
            std::fs::metadata(Self::resolve(path))
                .map(|metadata| metadata.len())
                .map_err(map_io)
        }

        /// Durable whole-file replace: write a `.tmp` sibling, then `rename` it
        /// over the target. Overrides the trait default to add optional
        /// pretty-printing of `.json` payloads (the `diskfs-pretty` feature).
        fn write_atomic(path: &str, data: &[u8]) -> Result<(), FsError> {
            let full = Self::resolve(path);
            Self::ensure_parent(&full)?;
            #[cfg(feature = "diskfs-pretty")]
            let payload = Self::render(path, data);
            #[cfg(not(feature = "diskfs-pretty"))]
            let payload = std::borrow::Cow::Borrowed(data);
            let mut tmp = full.clone().into_os_string();
            tmp.push(".tmp");
            let tmp = PathBuf::from(tmp);
            std::fs::write(&tmp, payload.as_ref()).map_err(FsError::from)?;
            std::fs::rename(&tmp, &full).map_err(FsError::from)
        }
    }

    #[cfg(feature = "diskfs-pretty")]
    impl DiskFs {
        /// Pretty-print `.json` payloads when enabled; otherwise pass through.
        fn render<'data>(path: &str, data: &'data [u8]) -> std::borrow::Cow<'data, [u8]> {
            let pretty_json = CONFIG.with(|slot| slot.borrow().pretty_json);
            if pretty_json && path.ends_with(".json") {
                match serde_json::from_slice::<serde_json::Value>(data)
                    .ok()
                    .and_then(|value| serde_json::to_vec_pretty(&value).ok())
                {
                    Some(pretty) => std::borrow::Cow::Owned(pretty),
                    None => std::borrow::Cow::Borrowed(data),
                }
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
