//! The per-agent [`SkillSet`]: which skills are loaded into context, plus the
//! two borrowed prompt fragments the agent reads.
//!
//! - [`catalog`](SkillSet::catalog) — every *available* skill as a one-line
//!   menu (id + description), so the model knows what it can load.
//! - [`context`](SkillSet::context) — the full bodies of the skills currently
//!   *loaded*, assembled into one fragment.
//!
//! A `SkillSet` is **mutable**: skills load and unload at runtime without
//! restarting the agent. Both fragments are **dirty-cached** — reads when
//! nothing changed are O(1) and hand back a borrowed `&str`. The catalog cache
//! is keyed on the registry's current [`CatalogSnapshot`] identity, so a
//! [`reload`](crate::FsSkillRegistry::reload) that swaps in a new snapshot
//! invalidates it; the context cache is rebuilt whenever the loaded set changes.

use std::sync::Arc;

use super::registry::{CatalogSnapshot, SkillRegistry};
use super::skill::{SkillError, SkillId};

/// A named bundle of skills to load together (parallels `ToolGroup`).
pub struct SkillGroup {
    name: &'static str,
    skills: Vec<SkillId>,
}

impl SkillGroup {
    /// Bundle `skills` under the group identity `name`.
    pub fn new(name: &'static str, skills: impl IntoIterator<Item = SkillId>) -> Self {
        Self {
            name,
            skills: skills.into_iter().collect(),
        }
    }

    /// The group's identity.
    pub fn name(&self) -> &'static str {
        self.name
    }
}

/// One loaded skill plus the group it was loaded under.
struct Loaded {
    group: &'static str,
    id: SkillId,
}

/// The agent's loaded skills and their cached, assembled prompt fragments.
pub struct SkillSet {
    registry: Arc<dyn SkillRegistry>,
    loaded: Vec<Loaded>,
    /// Rendered available-skills menu, paired with the snapshot it was rendered
    /// from so a registry reload (new snapshot identity) invalidates it.
    catalog_cache: Option<(Arc<CatalogSnapshot>, String)>,
    /// Assembled context for the loaded skills; valid only when `!dirty`.
    context_cache: String,
    dirty: bool,
}

impl SkillSet {
    /// An empty set over `registry` — no skills loaded yet.
    pub fn new(registry: Arc<dyn SkillRegistry>) -> Self {
        Self {
            registry,
            loaded: Vec::new(),
            catalog_cache: None,
            context_cache: String::new(),
            dirty: false,
        }
    }

    /// True when no skills are loaded.
    pub fn is_empty(&self) -> bool {
        self.loaded.is_empty()
    }

    /// A handle to the catalog source this set reads from (a cheap `Arc` clone).
    ///
    /// Lets a runtime skill tool validate an id and render the available-skills
    /// menu against the same registry without taking a borrow on the set.
    pub fn registry(&self) -> Arc<dyn SkillRegistry> {
        Arc::clone(&self.registry)
    }

    /// Load one skill under `group`. No-op if already loaded.
    ///
    /// # Errors
    ///
    /// [`SkillError::NotFound`] if the registry has no such skill, so a bad id
    /// surfaces here rather than at the next context rebuild.
    pub fn load(&mut self, group: &'static str, id: SkillId) -> Result<(), SkillError> {
        if self.registry.metadata(&id).is_none() {
            return Err(SkillError::NotFound(id));
        }
        if self.loaded.iter().any(|loaded| loaded.id == id) {
            return Ok(());
        }
        self.loaded.push(Loaded { group, id });
        self.dirty = true;
        Ok(())
    }

    /// Load every skill in `group`.
    ///
    /// # Errors
    ///
    /// [`SkillError::NotFound`] for the first skill in the group the registry
    /// does not have (later skills in the group are not loaded).
    pub fn load_group(&mut self, group: SkillGroup) -> Result<(), SkillError> {
        let name = group.name;
        for id in group.skills {
            self.load(name, id)?;
        }
        Ok(())
    }

    /// Unload a skill by id. No-op if it was not loaded.
    pub fn unload(&mut self, id: &SkillId) {
        let before = self.loaded.len();
        self.loaded.retain(|loaded| &loaded.id != id);
        if self.loaded.len() != before {
            self.dirty = true;
        }
    }

    /// The available-skills menu (every catalog entry as `- <id>: <description>`),
    /// borrowed from a cache.
    ///
    /// `&mut self` so the cache can be filled in place. The cache is keyed on the
    /// registry's current snapshot identity: a [`reload`](crate::FsSkillRegistry::reload)
    /// swaps in a new [`CatalogSnapshot`], which the [`Arc::ptr_eq`] check below
    /// detects, triggering a re-render.
    pub fn catalog(&mut self) -> &str {
        let snapshot = self.registry.catalog();
        let fresh = self
            .catalog_cache
            .as_ref()
            .is_some_and(|(cached, _)| Arc::ptr_eq(cached, &snapshot));
        if !fresh {
            let rendered = render_catalog(&snapshot);
            self.catalog_cache = Some((snapshot, rendered));
        }
        self.catalog_cache
            .as_ref()
            .map(|(_, rendered)| rendered.as_str())
            .unwrap_or_default()
    }

    /// The assembled context for the loaded skills, rebuilt only if stale.
    ///
    /// Borrowed from the internal cache — no owned copy is handed out. `&mut self`
    /// because a stale cache is rebuilt in place; the agent calls this inside its
    /// `tick(&mut self)`.
    ///
    /// Unlike [`catalog`](Self::catalog), this cache tracks the **loaded set**,
    /// not the registry snapshot: it is invalidated by [`load`](Self::load) /
    /// [`unload`](Self::unload), *not* by a [`reload`](crate::FsSkillRegistry::reload).
    /// That is deliberate — each fragment tracks the state it projects (this one
    /// projects what the agent loaded, the menu projects what the registry offers).
    /// Consequence: if a loaded skill's document changes or is removed on disk and
    /// the registry reloads, this keeps serving the cached body until the next
    /// load/unload forces a rebuild (at which point a now-missing skill surfaces as
    /// [`SkillError::NotFound`]).
    ///
    /// # Errors
    ///
    /// Any [`SkillError`] from [`SkillRegistry::write_document`] when a loaded
    /// skill's document is (re)read during a rebuild — e.g. [`SkillError::ReadFailed`]
    /// or a malformed-front-matter error. On error `dirty` stays set so the next
    /// read retries (the partially written buffer is cleared and rebuilt).
    ///
    /// The buffer is **reused** across rebuilds: `clear()` retains its capacity,
    /// and each document body is appended straight into it via
    /// [`SkillRegistry::write_document`], so a rebuild does not free the previous
    /// buffer nor allocate a `String` per document.
    pub fn context(&mut self) -> Result<&str, SkillError> {
        if self.dirty {
            // Borrow disjoint fields: `&self.loaded` (iter), `&self.registry`,
            // and `&mut self.context_cache` are distinct fields, so this is a
            // single in-place rebuild with no temporary buffer.
            self.context_cache.clear();
            for loaded in &self.loaded {
                self.context_cache.push_str("## Skill: ");
                self.context_cache.push_str(loaded.id.as_str());
                self.context_cache.push_str(" (");
                self.context_cache.push_str(loaded.group);
                self.context_cache.push_str(")\n\n");
                let body_start = self.context_cache.len();
                self.registry
                    .write_document(&loaded.id, &mut self.context_cache)?;
                // Trim the just-appended body's trailing whitespace in place
                // (real `SKILL.md` bodies carry no leading blank line) without
                // allocating an intermediate trimmed `String`.
                let trimmed_len = self.context_cache.trim_end().len().max(body_start);
                self.context_cache.truncate(trimmed_len);
                self.context_cache.push_str("\n\n");
            }
            self.dirty = false;
        }
        Ok(&self.context_cache)
    }
}

/// Render the catalog as a one-line-per-skill menu for the prompt.
fn render_catalog(snapshot: &CatalogSnapshot) -> String {
    let mut out = String::from("Available skills:\n");
    for metadata in snapshot.entries() {
        out.push_str("- ");
        out.push_str(metadata.id().as_str());
        out.push_str(": ");
        out.push_str(metadata.description());
        out.push('\n');
    }
    out
}
