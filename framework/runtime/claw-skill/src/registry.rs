//! Filesystem-backed skill registry.

use std::marker::PhantomData;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, PoisonError, RwLock, RwLockReadGuard, RwLockWriteGuard};

use claw_interface::{ClawFs, FsError};

use super::skill::{front_matter_sections, parse_front_matter, Skill, SkillError, SkillId};
use super::skill_set::SkillSet;

pub type SkillRegistryVersion = u32;

const METADATA_PREFIX_BYTES: u64 = 2048;
const CUR_SKILL_DIR_PLACEHOLDER: &str = "{CUR_SKILL_DIR}";

/// Immutable point-in-time catalog view.
#[derive(Debug)]
pub struct CatalogSnapshot {
    version: SkillRegistryVersion,
    skills: Arc<[Skill]>,
}

impl CatalogSnapshot {
    pub(crate) fn empty() -> Self {
        Self {
            version: 0,
            skills: Arc::from([]),
        }
    }

    /// Snapshot version, bumped by every successful registry reload.
    pub fn version(&self) -> SkillRegistryVersion {
        self.version
    }

    /// Skills sorted by id, with root priority already resolved.
    pub fn skills(&self) -> &[Skill] {
        &self.skills
    }

    /// Look up one skill by id.
    pub fn get(&self, id: &SkillId) -> Option<&Skill> {
        self.skills.iter().find(|skill| skill.id() == id)
    }
}

/// Minimal public registry surface used by agent resolvers.
pub trait SkillRegistry: Send + Sync {
    /// Create a per-agent [`SkillSet`] projection backed by this registry.
    fn skill_set(self: Arc<Self>) -> SkillSet;
}

pub(crate) trait SkillRegistryBackend: Send + Sync {
    fn catalog(&self) -> Arc<CatalogSnapshot>;
    fn reload(&self) -> Result<(), SkillError>;
    fn load_document_into(&self, id: &SkillId, out: &mut String) -> Result<(), SkillError>;
}

/// Empty registry used by agents without skill backing.
#[derive(Debug, Default)]
pub struct EmptySkillRegistry;

impl SkillRegistry for EmptySkillRegistry {
    fn skill_set(self: Arc<Self>) -> SkillSet {
        SkillSet::from_registry(self)
    }
}

impl SkillRegistryBackend for EmptySkillRegistry {
    fn catalog(&self) -> Arc<CatalogSnapshot> {
        Arc::new(CatalogSnapshot::empty())
    }

    fn reload(&self) -> Result<(), SkillError> {
        Ok(())
    }

    fn load_document_into(&self, id: &SkillId, _out: &mut String) -> Result<(), SkillError> {
        Err(SkillError::NotFound(id.clone()))
    }
}

/// Filesystem-backed registry over one or more priority-ordered skill roots.
pub struct FsSkillRegistry<F: ClawFs> {
    roots: Vec<String>,
    snapshot: RwLock<Arc<CatalogSnapshot>>,
    next_version: AtomicU32,
    _fs: PhantomData<fn() -> F>,
}

impl<F: ClawFs> FsSkillRegistry<F> {
    /// Create an empty registry builder.
    pub fn new() -> Self {
        Self {
            roots: Vec::new(),
            snapshot: RwLock::new(Arc::new(CatalogSnapshot::empty())),
            next_version: AtomicU32::new(1),
            _fs: PhantomData,
        }
    }

    /// Append one skills root, rescan, and return the registry builder.
    ///
    /// Add roots in priority order, e.g. DATA before SYSTEM.
    pub fn set_root(mut self, root: impl Into<String>) -> Result<Self, SkillError> {
        self.roots.push(root.into());
        let snapshot = self.scan_catalog_next_version()?;
        *self.write_snapshot() = Arc::new(snapshot);
        Ok(self)
    }

    /// Create a per-agent [`SkillSet`] projection backed by this registry.
    pub fn skill_set(self: &Arc<Self>) -> SkillSet
    where
        F: 'static,
    {
        let registry: Arc<dyn SkillRegistryBackend> = self.clone();
        SkillSet::from_registry(registry)
    }

    pub(crate) fn catalog(&self) -> Arc<CatalogSnapshot> {
        Arc::clone(&self.read_snapshot())
    }

    pub(crate) fn reload(&self) -> Result<(), SkillError> {
        let snapshot = self.scan_catalog_next_version()?;
        *self.write_snapshot() = Arc::new(snapshot);
        Ok(())
    }

    pub(crate) fn load_document_into(
        &self,
        id: &SkillId,
        out: &mut String,
    ) -> Result<(), SkillError> {
        let snapshot = self.catalog();
        let skill = snapshot
            .get(id)
            .ok_or_else(|| SkillError::NotFound(id.clone()))?;
        let skill_dir = skill_directory_path(&skill.root, id.as_str());
        let path = format!("{skill_dir}/SKILL.md");
        let bytes = self
            .read_skill_document(&path)
            .map_err(|error| SkillError::ReadFailed(id.clone(), error))?;
        let text = String::from_utf8(bytes).map_err(|_| SkillError::InvalidUtf8(id.clone()))?;
        let (_, body) = front_matter_sections(id, &text)?;
        append_wrapped_document(id, body, &skill_dir, out);
        Ok(())
    }

    fn scan_catalog_next_version(&self) -> Result<CatalogSnapshot, SkillError> {
        let version = self.next_version.fetch_add(1, Ordering::Relaxed);
        scan_catalog::<F>(&self.roots, version)
    }

    fn read_skill_document(&self, path: &str) -> Result<Vec<u8>, FsError> {
        F::read(path)
    }

    fn read_snapshot(&self) -> RwLockReadGuard<'_, Arc<CatalogSnapshot>> {
        self.snapshot.read().unwrap_or_else(PoisonError::into_inner)
    }

    fn write_snapshot(&self) -> RwLockWriteGuard<'_, Arc<CatalogSnapshot>> {
        self.snapshot
            .write()
            .unwrap_or_else(PoisonError::into_inner)
    }
}

impl<F: ClawFs> Default for FsSkillRegistry<F> {
    fn default() -> Self {
        Self::new()
    }
}

impl<F: ClawFs + 'static> SkillRegistry for FsSkillRegistry<F> {
    fn skill_set(self: Arc<Self>) -> SkillSet {
        let registry: Arc<dyn SkillRegistryBackend> = self;
        SkillSet::from_registry(registry)
    }
}

impl<F: ClawFs> SkillRegistryBackend for FsSkillRegistry<F> {
    fn catalog(&self) -> Arc<CatalogSnapshot> {
        FsSkillRegistry::catalog(self)
    }

    fn reload(&self) -> Result<(), SkillError> {
        FsSkillRegistry::reload(self)
    }

    fn load_document_into(&self, id: &SkillId, out: &mut String) -> Result<(), SkillError> {
        FsSkillRegistry::load_document_into(self, id, out)
    }
}

fn scan_catalog<F: ClawFs>(
    roots: &[String],
    version: SkillRegistryVersion,
) -> Result<CatalogSnapshot, SkillError> {
    let mut skills = Vec::new();
    for root in roots {
        let names = match F::list_dir(root) {
            Ok(names) => names,
            Err(FsError::NotFound) => continue,
            Err(error) => return Err(SkillError::ScanFailed(root.clone(), error)),
        };
        for name in names {
            let id = SkillId::new(name);
            if skills.iter().any(|skill: &Skill| skill.id() == &id) {
                continue;
            }
            let path = skill_document_path(root, id.as_str());
            if !F::exists(&path) {
                continue;
            }
            let head = read_head::<F>(&id, &path)?;
            skills.push(parse_front_matter(id, root, &head)?);
        }
    }
    skills.sort_by(|left, right| left.id().cmp(right.id()));
    Ok(CatalogSnapshot {
        version,
        skills: Arc::from(skills),
    })
}

fn skill_document_path(root: &str, id: &str) -> String {
    format!("{}/SKILL.md", skill_directory_path(root, id))
}

fn skill_directory_path(root: &str, id: &str) -> String {
    format!("{}/{}", root.trim_end_matches('/'), id)
}

fn append_wrapped_document(id: &SkillId, body: &str, skill_dir: &str, out: &mut String) {
    out.push_str("<skill_content name=\"");
    append_xml_attribute_escaped(id.as_str(), out);
    out.push_str("\">\n");
    append_with_cur_skill_dir_expanded(body.trim(), skill_dir, out);
    out.push_str("\n</skill_content>");
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

fn append_xml_attribute_escaped(text: &str, out: &mut String) {
    for ch in text.chars() {
        match ch {
            '&' => out.push_str("&amp;"),
            '"' => out.push_str("&quot;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '\'' => out.push_str("&apos;"),
            _ => out.push(ch),
        }
    }
}

fn read_head<F: ClawFs>(id: &SkillId, path: &str) -> Result<String, SkillError> {
    let read_failed = |error| SkillError::ReadFailed(id.clone(), error);
    let size = F::len(path).map_err(read_failed)?;
    let take = size.min(METADATA_PREFIX_BYTES) as usize;
    let bytes = F::read_at(path, 0, take).map_err(read_failed)?;
    String::from_utf8(bytes).map_err(|_| SkillError::InvalidUtf8(id.clone()))
}
