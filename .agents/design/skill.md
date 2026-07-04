# Skill

```rust
type SkillId = String
type SkillRegistryVersion = u32

// A skill is an immutable snapshot discovered from skills/<id>/SKILL.md.
// Filesystem writes are observed only after SkillRegistry::reload().
struct Skill {
    id: SkillId // directory name; must match SKILL.md frontmatter name when present
    name: String // frontmatter name; validation requires it to match id
    description: String // one-line catalog text shown to the model
    author: Option<String>
    metadata: SkillFrontmatterMetadata // cap_groups/manage_mode/category/peripherals/tags

    pub fn id(&self) -> &str
    pub fn name(&self) -> &str
    pub fn description(&self) -> &str
}

struct CatalogSnapshot {
    registry_version: SkillRegistryVersion
    skills: Vec<Skill> // sorted by id; root priority already resolved

    pub fn version(&self) -> SkillRegistryVersion
    pub fn skills(&self) -> &[Skill]
}

trait SkillRegistry: Send + Sync {
    fn catalog(&self) -> Arc<CatalogSnapshot> // cheap snapshot clone; never scans
    fn reload(&self) -> Result<()> // rereads FS after external writes; never writes skills
}

struct FsSkillRegistry<F: ClawFs> {
    fs: F
    roots: Vec<String> // priority ordered: DATA before SYSTEM
    snapshot: RwLock<Arc<CatalogSnapshot>>
    next_version: Atomic<SKillRegistryVersion>

    pub fn scan(fs: F, root: impl Into<String>) -> Result<FsSkillRegistry<F>>
    pub fn scan_roots(fs: F, roots: impl IntoIterator<Item = impl Into<String>>) -> Result<FsSkillRegistry<F>>
    pub fn reload(&self) -> Result<()> // read roots, build full snapshot, then atomically swap
}

mod bake {
    pub fn validate_skills_dir(skills_dir: &Path) -> Result<usize> // build-time check for skills/<id>/SKILL.md
}

struct LoadedSkill {
    group: String // "manifest", "model", adapter id, etc.
    id: SkillId
}

struct SkillSetCache {
    catalog_context: Option<String>, // "Available skills:\n- id: description\n..."
    loaded_context: Option<String>, // full bodies of loaded skills
}

struct SkillSet {
    registry: Arc<dyn SkillRegistry>,
    loaded: Vec<LoadedSkill>,
    cache: SkillSetCache,
    loaded_version: SkillRegistryVersion,
    should_rebuild_catalog: bool,
    should_rebuild_loaded: bool,

    // Existing semantics preserved: loading validates the id; duplicate load is a no-op.
    pub fn load(&mut self, id: SkillId) -> Result<()>
    pub fn load_group(&mut self, group: SkillGroup) -> Result<()>
    pub fn unload(&mut self, id: &SkillId) -> Result<()> // no-op if already unloaded
    pub fn unload_group(&mut self, group: SkillGroup) -> Result<()> // unloads the group's ids; no-op for ids not loaded

    pub fn begin(&mut self) -> Result<SkillSetHandle> // observes current catalog snapshot, rebuilds dirty caches, freezes view

    fn rebuild_catalog_context(snapshot: &CatalogSnapshot)
    fn rebuild_loaded_context(snapshot: &CatalogSnapshot) -> Result<()>
}

struct SkillSetHandle {
    pub fn catalog_context() -> &str // stable during this handle
    pub fn loaded_context() -> &str // stable during this handle
}

struct SkillGroup {
    pub fn new(group: impl Into<String>, skills: impl IntoIterator<Item = SkillId>) -> SkillGroup
    pub fn name(&self) -> &str
    pub fn skills(&self) -> &[SkillId]
}

enum SkillError {
    NotFound(SkillId)
    ScanFailed(String, FsError)
    ReadFailed(SkillId, FsError)
    InvalidUtf8(SkillId)
    MissingOpeningFence(SkillId)
    MissingClosingFence(SkillId)
    InvalidJson(SkillId, String)
    InvalidFrontmatter(SkillId, String)
}
```

Notes:

- Skills are written, updated, or removed through the filesystem, not through `claw-skill` APIs or skill tool calls. Writers can be the web UI, installer, recovery copy, tests, or any other FS owner.
- `SkillRegistry` is filesystem-backed catalog/document access. It has no create/update/delete skill API; after FS writes, callers use `reload()` to make the new filesystem state visible.
- The registry trait is intentionally small because the skill tools only need `catalog()` for `list_skills` and `reload()` for `reload_skills`. `SkillSet` rebuilds loaded skill bodies from the current `CatalogSnapshot`.
- `reload()` is the only registry state-change path: read roots, validate frontmatter, resolve root priority, then atomically swap a new `CatalogSnapshot`. It does not write skill files.
- `SkillSet` only owns per-agent loaded skill ids and prompt caches. Loading/unloading skills changes the loaded skill body context only; it never changes the registry or filesystem.
- A loaded skill is resident in this `SkillSet`'s context projection. `load()` is not a one-shot document return; every later `begin()` includes the loaded skill bodies until `unload()` / `unload_group()` removes them or the `SkillSet` is dropped.
- `load()` validates against the current `CatalogSnapshot`. If a skill was just written to FS, callers must `reload()` before loading it.
- `SkillSet::begin()` is the request/iteration boundary. It observes the current catalog snapshot and freezes `catalog_context` + `loaded_context` for that request.
- Registry version changes invalidate both catalog and loaded-context caches. If a loaded skill disappeared after `reload()`, the next loaded-context rebuild returns `NotFound`.
- The filesystem registry owns root priority and `{CUR_SKILL_DIR}` expansion. DATA roots should precede SYSTEM roots.
