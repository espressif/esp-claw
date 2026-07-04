# Skill

```rust
type SkillId = String
type SkillRegistryVersion = u32

enum SkillManageMode {
    Readonly, // "readonly"; "web" is accepted in SKILL.md and normalized to Readonly on device
    Runtime, // "runtime"
}

struct SkillFrontmatterMetadata {
    cap_groups: Vec<String> // parsed and preserved; tool visibility is not wired in this design pass
    manage_mode: SkillManageMode
    category: Vec<String>
    peripherals: Vec<String>
    tags: Vec<String>
}

struct Skill {
    id: SkillId // directory name; must match SKILL.md frontmatter name
    name: String
    description: String
    author: Option<String>
    metadata: SkillFrontmatterMetadata
    file: String // relative document path, "<id>/SKILL.md"

    pub fn id(&self) -> &str
    pub fn name(&self) -> &str
    pub fn description(&self) -> &str
    pub fn file(&self) -> &str
    pub fn metadata(&self) -> &SkillFrontmatterMetadata
}

struct CatalogSnapshot {
    version: SkillRegistryVersion
    skills: Arc<[Skill]> // sorted by id; root priority already resolved

    pub fn version(&self) -> SkillRegistryVersion
    pub fn skills(&self) -> &[Skill]
    pub fn get(&self, id: &SkillId) -> Option<&Skill>
}

struct SkillDocument {
    content: Arc<str> // <skill_content name="id">\n...stripped body...\n</skill_content>

    pub fn content(&self) -> &str
}

struct FsSkillRegistry<F: ClawFs> {
    fs: F
    roots: Vec<String> // priority ordered: DATA before SYSTEM
    snapshot: RwLock<Arc<CatalogSnapshot>>
    next_version: Atomic<SkillRegistryVersion>

    pub fn new(fs: F) -> FsSkillRegistry<F>
    pub fn set_root(self, root: impl Into<String>) -> Result<FsSkillRegistry<F>> // appends root, reloads snapshot, returns self
    pub fn skill_set(self: &Arc<Self>) -> SkillSet

    fn catalog(&self) -> Arc<CatalogSnapshot> // cheap snapshot clone; never scans
    fn reload(&self) -> Result<()> // rereads FS after external writes; never writes skills
    fn load_document_into(&self, id: &SkillId, out: &mut String) -> Result<()> // strips frontmatter, expands {CUR_SKILL_DIR}, wraps XML
}

struct SkillSet {
    registry: Arc<FsSkillRegistry<impl ClawFs>>,
    catalog_version: SkillRegistryVersion,
    catalog_buffer: String,
    document_buffer: String,

    pub fn reload(&self) -> Result<()> // calls registry.reload(); next catalog_context rerenders
    pub fn list_skill(&mut self) -> Result<Arc<str>> // JSON catalog; reuses catalog_buffer
    pub fn catalog_context(&mut self) -> &str // prompt text; reuses catalog_buffer while snapshot version is unchanged
    pub fn activate_skill(&mut self, id: &SkillId) -> Result<SkillDocument> // reuses document_buffer, returns shared immutable content
}

mod bake {
    pub fn validate_skills_dir(skills_dir: &Path) -> Result<usize> // build-time check for skills/<id>/SKILL.md
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

- Skills are filesystem-owned. They are written, updated, or removed through FS owners such as the web UI, installer, recovery copy, or tests, not through `claw-skill` APIs.
- Registry caller-facing API is intentionally small: create a registry, repeatedly `set_root(...)`, then get a `SkillSet` with `skill_set()`. Catalog/reload/document loading are registry internals used by `SkillSet`.
- Roots are priority ordered. Add DATA roots before SYSTEM roots so DATA skills shadow firmware-baked skills with the same id.
- `SkillSet` is the agent-facing cache and tool surface. Context adapters and skill tools should receive the shared `SkillSet`, not the registry.
- Share one `SkillSet` between the context adapter and skill tools, typically as `Arc<Mutex<SkillSet>>`, so `catalog_buffer` and `document_buffer` are reused.
- `CatalogSnapshot` stores structured skill metadata only, with `Arc<[Skill]>` so cloning `Arc<CatalogSnapshot>` never clones the skill list. Rendered prompt text and JSON list output live in `SkillSet::catalog_buffer`.
- `list_skill()` returns JSON catalog, matching master behavior. `catalog_context()` returns prompt text such as `Available skills:\n- id: description\n...`.
- `activate_skill()` strips `SKILL.md` frontmatter, expands `{CUR_SKILL_DIR}`, wraps the result like master's `<skill_content name="skill_id">...</skill_content>`, and returns immutable shared content.
- `SkillDocument` is immutable and shareable so tool callers can avoid holding a `SkillSet` lock while formatting or returning content.
- `activate_skill()` is a one-shot document load for the current tool result/context flow. It does not create persistent loaded-skill state in this design pass.
- `SKILL.md` frontmatter is a JSON object wrapped by `---`. Master requires `name`, `description`, and `metadata`; `author` is optional.
- `metadata.manage_mode` accepts `readonly`, `web`, or `runtime`; device runtime treats `web` as `readonly`.
- `metadata.cap_groups` is parsed and retained but tool visibility is intentionally not connected in this design pass.
