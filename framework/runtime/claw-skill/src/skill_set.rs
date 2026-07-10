//! Per-agent skill projection and reusable render buffers.

use std::fmt::Write as _;
use std::sync::Arc;

use super::registry::{
    CatalogSnapshot, EmptySkillRegistry, SkillRegistryBackend, SkillRegistryVersion,
};
use super::skill::{SkillDocument, SkillError, SkillId};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CatalogBufferKind {
    Empty,
    ListJson,
    Context,
}

/// Per-agent skill view and cache.
pub struct SkillSet {
    registry: Arc<dyn SkillRegistryBackend>,
    catalog_version: SkillRegistryVersion,
    catalog_buffer_kind: CatalogBufferKind,
    catalog_buffer: String,
    document_buffer: String,
}

impl SkillSet {
    /// A skill set over an empty registry.
    pub fn empty() -> Self {
        Self::from_registry(Arc::new(EmptySkillRegistry))
    }

    pub(crate) fn from_registry(registry: Arc<dyn SkillRegistryBackend>) -> Self {
        Self {
            registry,
            catalog_version: 0,
            catalog_buffer_kind: CatalogBufferKind::Empty,
            catalog_buffer: String::new(),
            document_buffer: String::new(),
        }
    }

    /// Re-scan the backing registry. The next catalog render observes the new
    /// snapshot version and refreshes its cache.
    pub fn reload(&self) -> Result<(), SkillError> {
        self.registry.reload()
    }

    /// JSON catalog for tool output. The returned borrow is valid until the next
    /// mutable method call on this `SkillSet`.
    pub fn list_skill(&mut self) -> Result<&str, SkillError> {
        let snapshot = self.registry.catalog();
        if !self.catalog_cache_is_fresh(&snapshot, CatalogBufferKind::ListJson) {
            self.render_list_json(&snapshot);
        }
        Ok(&self.catalog_buffer)
    }

    /// Prompt-facing catalog summary. The returned borrow is valid until the
    /// next mutable method call on this `SkillSet`.
    pub fn catalog_context(&mut self) -> &str {
        let snapshot = self.registry.catalog();
        if !self.catalog_cache_is_fresh(&snapshot, CatalogBufferKind::Context) {
            self.render_catalog_context(&snapshot);
        }
        &self.catalog_buffer
    }

    /// Read and render one activated skill document.
    pub fn activate_skill(&mut self, id: &SkillId) -> Result<SkillDocument, SkillError> {
        self.document_buffer.clear();
        self.registry
            .load_document_into(id, &mut self.document_buffer)?;
        Ok(SkillDocument::new(self.document_buffer.clone()))
    }

    fn catalog_cache_is_fresh(&self, snapshot: &CatalogSnapshot, kind: CatalogBufferKind) -> bool {
        self.catalog_version == snapshot.version() && self.catalog_buffer_kind == kind
    }

    fn render_list_json(&mut self, snapshot: &CatalogSnapshot) {
        self.catalog_buffer.clear();
        self.catalog_buffer.push('[');
        for (index, skill) in snapshot.skills().iter().enumerate() {
            if index > 0 {
                self.catalog_buffer.push(',');
            }
            self.catalog_buffer.push('{');
            push_json_field(&mut self.catalog_buffer, "id", skill.id().as_str(), false);
            push_json_field(&mut self.catalog_buffer, "name", skill.name(), true);
            push_json_field(
                &mut self.catalog_buffer,
                "description",
                skill.description(),
                true,
            );
            if let Some(author) = skill.author() {
                push_json_field(&mut self.catalog_buffer, "author", author, true);
            }
            push_json_field(&mut self.catalog_buffer, "file", skill.file(), true);
            self.catalog_buffer.push_str(",\"metadata\":{");
            let manage_mode: &'static str = skill.metadata().manage_mode().into();
            push_json_array_field(
                &mut self.catalog_buffer,
                "cap_groups",
                skill.metadata().cap_groups(),
                false,
            );
            push_json_field(&mut self.catalog_buffer, "manage_mode", manage_mode, true);
            push_json_array_field(
                &mut self.catalog_buffer,
                "category",
                skill.metadata().category(),
                true,
            );
            push_json_array_field(
                &mut self.catalog_buffer,
                "peripherals",
                skill.metadata().peripherals(),
                true,
            );
            push_json_array_field(
                &mut self.catalog_buffer,
                "tags",
                skill.metadata().tags(),
                true,
            );
            self.catalog_buffer.push_str("}}");
        }
        self.catalog_buffer.push(']');
        self.catalog_version = snapshot.version();
        self.catalog_buffer_kind = CatalogBufferKind::ListJson;
    }

    fn render_catalog_context(&mut self, snapshot: &CatalogSnapshot) {
        self.catalog_buffer.clear();
        self.catalog_buffer.push_str("Available skills:\n");
        for skill in snapshot.skills() {
            self.catalog_buffer.push_str("- ");
            self.catalog_buffer.push_str(skill.id().as_str());
            self.catalog_buffer.push_str(": ");
            self.catalog_buffer.push_str(skill.description());
            self.catalog_buffer.push('\n');
        }
        self.catalog_version = snapshot.version();
        self.catalog_buffer_kind = CatalogBufferKind::Context;
    }
}

fn push_json_field(out: &mut String, key: &str, value: &str, comma: bool) {
    if comma {
        out.push(',');
    }
    push_json_string(out, key);
    out.push(':');
    push_json_string(out, value);
}

fn push_json_array_field(out: &mut String, key: &str, values: &[String], comma: bool) {
    if comma {
        out.push(',');
    }
    push_json_string(out, key);
    out.push_str(":[");
    for (index, value) in values.iter().enumerate() {
        if index > 0 {
            out.push(',');
        }
        push_json_string(out, value);
    }
    out.push(']');
}

fn push_json_string(out: &mut String, value: &str) {
    out.push('"');
    for ch in value.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            ch if ch.is_control() => {
                let _ = write!(out, "\\u{:04x}", ch as u32);
            }
            ch => out.push(ch),
        }
    }
    out.push('"');
}
