//! Skill registry: the catalog source the agent's [`SkillSet`] reads from.
//!
//! [`FsSkillRegistry`] scans one or more skills roots over an injected
//! [`ClawFs`]: one directory per skill, each holding a `SKILL.md`. The catalog
//! (metadata) is read by parsing only each file's front-matter head; full
//! documents are read lazily, on demand, when a skill is placed in context.
//!
//! Roots are scanned in priority order. If the same skill id appears in more
//! than one root, the first root wins and later copies are ignored. This keeps
//! the runtime model aligned with the firmware layout: writable/user skills can
//! shadow firmware-baked read-only skills without introducing a sandbox path
//! rewrite layer.
//!
//! The catalog is **mutable at runtime**: [`reload`](FsSkillRegistry::reload)
//! re-scans the roots and atomically swaps in a fresh [`CatalogSnapshot`].
//! Because the live state sits behind a lock, the read API hands out a cheap
//! `Arc<CatalogSnapshot>` *snapshot* rather than a borrow — a borrow could not
//! escape the lock guard, and a snapshot lets a concurrent reload proceed
//! without disturbing readers already holding an older view.
//!
//! [`SkillSet`]: crate::SkillSet

use std::collections::HashMap;
use std::sync::{Arc, RwLock};

use claw_interface::{ClawFs, FsError};

use super::skill::{parse_front_matter, strip_front_matter, SkillError, SkillId, SkillMetadata};

/// Maximum bytes read from a `SKILL.md` head to parse its front-matter.
///
/// Real metadata blocks are a few hundred bytes; this bound keeps the catalog
/// scan from reading large document bodies. A header that does not close within
/// this window is treated as malformed.
const METADATA_PREFIX_BYTES: u64 = 2048;
const CUR_SKILL_DIR_PLACEHOLDER: &str = "{CUR_SKILL_DIR}";

/// An immutable, point-in-time view of a registry's catalog.
///
/// Handed out (inside an [`Arc`]) by [`SkillRegistry::catalog`] so readers get a
/// consistent snapshot that a concurrent [`reload`](FsSkillRegistry::reload)
/// cannot mutate underneath them: a reload swaps in a *new* snapshot, leaving
/// any already-handed-out `Arc` untouched. Holds everything the read path needs
/// — the catalog rows plus the id → root map used to locate a document.
#[derive(Debug, Default)]
pub struct CatalogSnapshot {
    entries: Vec<SkillMetadata>,
    /// The root each skill id was found under, so a document read can rebuild
    /// its path.
    root_by_id: HashMap<SkillId, String>,
}

impl CatalogSnapshot {
    /// The catalog rows (id + description), sorted by id.
    pub fn entries(&self) -> &[SkillMetadata] {
        &self.entries
    }

    /// Look up one catalog entry by id, or `None` if there is no such skill.
    pub fn get(&self, id: &SkillId) -> Option<&SkillMetadata> {
        self.entries.iter().find(|entry| entry.id() == id)
    }

    /// The root directory `id` was found under, if any.
    fn root_of(&self, id: &SkillId) -> Option<&str> {
        self.root_by_id.get(id).map(String::as_str)
    }
}

/// The catalog + document source the agent's [`SkillSet`] reads from.
///
/// An inbound port: the catalog (cheap metadata) is held in memory, while full
/// documents are fetched on demand. [`FsSkillRegistry`] is the filesystem-backed
/// implementation; tests can substitute a fake.
///
/// [`SkillSet`]: crate::SkillSet
pub trait SkillRegistry: Send + Sync {
    /// A consistent snapshot of the catalog, shared via [`Arc`].
    ///
    /// Cheap to call (an `Arc` clone): the returned snapshot is immutable, so a
    /// concurrent [`reload`](FsSkillRegistry::reload) replaces the live snapshot
    /// without disturbing a caller still reading this one.
    fn catalog(&self) -> Arc<CatalogSnapshot>;

    /// Append one skill's document body (front-matter stripped) to `out`.
    ///
    /// The allocation-frugal primitive: the body is pushed straight into the
    /// caller's buffer, so a [`SkillSet`](crate::SkillSet) can reuse one buffer
    /// across rebuilds instead of allocating (and freeing) a fresh `String` per
    /// document.
    ///
    /// # Errors
    ///
    /// - [`SkillError::NotFound`] when no skill has `id`.
    /// - [`SkillError::ReadFailed`] / [`SkillError::InvalidUtf8`] if the file
    ///   cannot be read or decoded.
    /// - [`SkillError::MissingOpeningFence`] / [`SkillError::MissingClosingFence`]
    ///   if the document's front-matter is malformed.
    fn write_document(&self, id: &SkillId, out: &mut String) -> Result<(), SkillError>;

    /// One skill's full document body (front-matter stripped), as an owned
    /// `String`.
    ///
    /// Convenience over [`write_document`](Self::write_document) for callers
    /// that want an owned value; allocation-sensitive callers should prefer
    /// `write_document` into a reused buffer.
    ///
    /// # Errors
    ///
    /// Same as [`write_document`](Self::write_document).
    fn document(&self, id: &SkillId) -> Result<String, SkillError> {
        let mut out = String::new();
        self.write_document(id, &mut out)?;
        Ok(out)
    }

    /// Look up one catalog entry by id, or `None` if there is no such skill.
    ///
    /// Returns an owned [`SkillMetadata`]: the live catalog sits behind a lock,
    /// so a borrow could not outlive the snapshot it was read from.
    fn metadata(&self, id: &SkillId) -> Option<SkillMetadata> {
        self.catalog().get(id).cloned()
    }

    /// Re-scan the catalog source and atomically swap in a fresh view, so skills
    /// added to (or removed from) the source since construction become visible.
    ///
    /// Default: a no-op, for sources whose catalog is immutable (a static or
    /// in-memory registry has nothing to re-scan). The filesystem-backed
    /// [`FsSkillRegistry`] overrides this to re-scan its roots.
    ///
    /// # Errors
    ///
    /// Implementation-defined. [`FsSkillRegistry`] returns the same errors as
    /// [`scan_roots`](FsSkillRegistry::scan_roots); on error the previous catalog
    /// is left in place.
    fn reload(&self) -> Result<(), SkillError> {
        Ok(())
    }
}

/// A registry with an empty catalog and no documents.
///
/// Used when an agent has no skill backing. This keeps "no skills" represented as
/// an empty [`SkillSet`](crate::SkillSet) instead of an `Option<SkillSet>` while
/// still surfacing requested skill ids as [`SkillError::NotFound`].
#[derive(Debug, Default)]
pub struct EmptySkillRegistry;

impl SkillRegistry for EmptySkillRegistry {
    fn catalog(&self) -> Arc<CatalogSnapshot> {
        Arc::new(CatalogSnapshot::default())
    }

    fn write_document(&self, id: &SkillId, _out: &mut String) -> Result<(), SkillError> {
        Err(SkillError::NotFound(id.clone()))
    }
}

/// A [`SkillRegistry`] backed by one or more [`ClawFs`] skills directories.
///
/// The scanned catalog lives behind an `RwLock<Arc<…>>` so it can be replaced at
/// runtime by [`reload`](Self::reload) — even while shared as an
/// `Arc<dyn SkillRegistry>` — without handing out a borrow that would outlive a
/// reload. `fs` and `roots` are fixed at construction and need no locking.
///
/// The persistence backend `F` is a concrete, statically dispatched [`ClawFs`]
/// held by value (not behind `Arc<dyn>`): the device passes its single on-disk
/// implementation, host tools and tests pass `DiskFs`/`MemFs`. Document reads
/// compile down to direct (monomorphized) calls with no vtable.
pub struct FsSkillRegistry<F: ClawFs> {
    fs: F,
    roots: Vec<String>,
    /// The live catalog snapshot, swapped atomically on [`reload`](Self::reload).
    snapshot: RwLock<Arc<CatalogSnapshot>>,
}

impl<F: ClawFs> FsSkillRegistry<F> {
    /// Scan a single `root` and build the catalog.
    ///
    /// Convenience for the common single-root case; see [`scan_roots`](Self::scan_roots).
    ///
    /// # Errors
    ///
    /// See [`scan_roots`](Self::scan_roots).
    pub fn scan(fs: F, root: impl Into<String>) -> Result<Self, SkillError> {
        Self::scan_roots(fs, [root.into()])
    }

    /// Scan every `root` in order and build the merged catalog.
    ///
    /// Each immediate subdirectory holding a `SKILL.md` becomes a catalog entry
    /// (id = directory name). The catalog is sorted by id for stable output. A
    /// directory whose `SKILL.md` is missing is skipped.
    ///
    /// # Errors
    ///
    /// - [`SkillError::ScanFailed`] if a present root directory cannot be listed.
    /// - [`SkillError::ReadFailed`] / [`SkillError::InvalidUtf8`] if a skill's
    ///   head cannot be read or decoded.
    /// - [`SkillError::MissingOpeningFence`] / [`SkillError::MissingClosingFence`]
    ///   / [`SkillError::InvalidJson`] if a skill's front-matter is malformed.
    pub fn scan_roots(
        fs: F,
        roots: impl IntoIterator<Item = impl Into<String>>,
    ) -> Result<Self, SkillError> {
        let roots: Vec<String> = roots.into_iter().map(Into::into).collect();
        let snapshot = Self::scan_catalog(&fs, &roots)?;
        Ok(Self {
            fs,
            roots,
            snapshot: RwLock::new(Arc::new(snapshot)),
        })
    }

    /// Build a [`CatalogSnapshot`] by scanning every root once.
    fn scan_catalog(fs: &F, roots: &[String]) -> Result<CatalogSnapshot, SkillError> {
        let mut entries = Vec::new();
        let mut root_by_id: HashMap<SkillId, String> = HashMap::new();
        for root in roots {
            let names = match fs.list_dir(root) {
                Ok(names) => names,
                // A root may legitimately be absent on some firmware layouts.
                Err(FsError::NotFound) => continue,
                Err(error) => return Err(SkillError::ScanFailed(root.clone(), error)),
            };
            for name in names {
                let id = SkillId::new(name);
                // Earlier roots have higher priority; later same-id skills are
                // shadowed, matching the pre-Rust claw_skill implementation.
                if root_by_id.contains_key(&id) {
                    continue;
                }
                let path = skill_document_path(root, id.as_str());
                if !fs.exists(&path) {
                    continue;
                }
                let head = read_head(fs, &id, &path)?;
                let metadata = parse_front_matter(id.clone(), &head)?;
                root_by_id.insert(id, root.clone());
                entries.push(metadata);
            }
        }
        entries.sort_by(|a, b| a.id().cmp(b.id()));
        Ok(CatalogSnapshot {
            entries,
            root_by_id,
        })
    }
}

impl<F: ClawFs> SkillRegistry for FsSkillRegistry<F> {
    fn catalog(&self) -> Arc<CatalogSnapshot> {
        // Poisoning only means a writer panicked mid-swap; the data is still a
        // valid `Arc`, so recover it rather than propagating the panic.
        self.snapshot
            .read()
            .map(|guard| Arc::clone(&guard))
            .unwrap_or_else(|poisoned| Arc::clone(&poisoned.into_inner()))
    }

    fn write_document(&self, id: &SkillId, out: &mut String) -> Result<(), SkillError> {
        let snapshot = self.catalog();
        let root = snapshot
            .root_of(id)
            .ok_or_else(|| SkillError::NotFound(id.clone()))?;
        let skill_dir = skill_directory_path(root, id.as_str());
        let path = format!("{skill_dir}/SKILL.md");
        let bytes = self
            .fs
            .read(&path)
            .map_err(|error| SkillError::ReadFailed(id.clone(), error))?;
        // `from_utf8` reuses the read buffer's allocation (no extra alloc); the
        // body slice is then copied straight into the caller's buffer.
        let text = String::from_utf8(bytes).map_err(|_| SkillError::InvalidUtf8(id.clone()))?;
        append_with_cur_skill_dir_expanded(strip_front_matter(id, &text)?, &skill_dir, out);
        Ok(())
    }

    /// Re-scan every root and atomically swap in a fresh catalog (e.g. after
    /// skills are added or removed on disk).
    ///
    /// The new snapshot is built fully before the swap, so the live state is
    /// only replaced on success; on error the previous catalog stays in place.
    /// Readers holding an `Arc` from an earlier [`catalog`](SkillRegistry::catalog)
    /// keep seeing their (now older) view until they fetch again.
    ///
    /// Takes `&self`: the catalog lives behind a lock, so a shared
    /// `Arc<dyn SkillRegistry>` can be reloaded without exclusive access.
    ///
    /// # Errors
    ///
    /// Same as [`scan_roots`](Self::scan_roots).
    fn reload(&self) -> Result<(), SkillError> {
        let snapshot = Self::scan_catalog(&self.fs, &self.roots)?;
        // Recover from a poisoned lock: a prior writer panic doesn't corrupt the
        // `Arc`, and we're about to overwrite it wholesale anyway.
        let mut guard = self
            .snapshot
            .write()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        *guard = Arc::new(snapshot);
        Ok(())
    }
}

fn skill_document_path(root: &str, id: &str) -> String {
    format!("{}/SKILL.md", skill_directory_path(root, id))
}

fn skill_directory_path(root: &str, id: &str) -> String {
    format!("{}/{}", root.trim_end_matches('/'), id)
}

fn append_with_cur_skill_dir_expanded(body: &str, skill_dir: &str, out: &mut String) {
    let mut pieces = body.split(CUR_SKILL_DIR_PLACEHOLDER);
    if let Some(first) = pieces.next() {
        out.push_str(first);
    }
    for piece in pieces {
        out.push_str(skill_dir);
        out.push_str(piece);
    }
}

/// Read just the front-matter head of a `SKILL.md`.
///
/// Clamps the read to the file size so small files don't trip `read_at`'s
/// past-EOF error, and to [`METADATA_PREFIX_BYTES`] so large bodies aren't read.
fn read_head<F: ClawFs>(fs: &F, id: &SkillId, path: &str) -> Result<String, SkillError> {
    let read_failed = |error| SkillError::ReadFailed(id.clone(), error);
    let size = fs.len(path).map_err(read_failed)?;
    let take = usize::try_from(size.min(METADATA_PREFIX_BYTES)).unwrap_or(usize::MAX);
    let bytes = fs.read_at(path, 0, take).map_err(read_failed)?;
    String::from_utf8(bytes).map_err(|_| SkillError::InvalidUtf8(id.clone()))
}
