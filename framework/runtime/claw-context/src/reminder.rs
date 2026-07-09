//! Ephemeral reminders: the agent's per-request "nudge" channel.
//!
//! A reminder is a transient instruction appended to the **tail** of the
//! messages sent to the LLM for one request and **never persisted** to memory.
//! It is the home for volatile, per-request guidance — e.g. the soft-hide phase
//! note naming the tools permitted this phase — that must reach the model
//! without moving the cached system/history prefix (the production
//! "system-reminder" pattern).
//!
//! # Determinism
//!
//! Reminders are one of the three context homes (stable prose -> a context
//! `Block`; real conversation/tool events -> persisted memory; per-request
//! transient nudges -> here). Nothing else may inject into a request tail.
//!
//! # Memory
//!
//! The rendered messages live in a **reused buffer** rebuilt only when the
//! reminder set changes (dirty-gated), so a steady reminder set costs nothing
//! per iteration. Each reminder renders once as a trailing `user` message
//! wrapped in a `<system-reminder>` envelope.
//!
//! Owned by [`Context`](crate::Context): callers reach it only through
//! [`Context::reminder`](crate::Context::reminder),
//! [`Context::with_reminder`](crate::Context::with_reminder), and the tail it
//! contributes to [`Context::request`](crate::Context::request).

use std::collections::BTreeMap;

use serde_json::{json, Value};

use crate::block::BlockKind;

/// The agent's ephemeral reminder channel. Holds the source texts plus a reused
/// render buffer; call [`refresh`](Self::refresh) once per tick before reading
/// [`as_slice`](Self::as_slice).
pub(crate) struct Reminders {
    /// Source reminder texts, keyed by context kind. The single source of truth.
    texts: BTreeMap<BlockKind, String>,
    /// Reused render buffer: one trailing `user` message per text, rebuilt only
    /// when `dirty`.
    rendered: Vec<Value>,
    /// The render buffer is stale relative to `texts`.
    dirty: bool,
}

impl Reminders {
    /// An empty reminder channel.
    pub(crate) fn new() -> Self {
        Self {
            texts: BTreeMap::new(),
            rendered: Vec::new(),
            dirty: false,
        }
    }

    /// Set one reminder `kind` to `text`, or clear it when `None`.
    ///
    /// **Dirty-gated:** a no-op (no re-render) when the resulting reminder is
    /// unchanged from the current one, so callers may re-derive and call this
    /// every tick (the steady case) at zero cost. Takes `&str` so the caller can
    /// pass a borrow of the source (e.g. the tool set's cached phase note) without
    /// owning it; the text is copied only when it actually changed.
    pub(crate) fn set(&mut self, kind: BlockKind, text: Option<&str>) {
        let text = text.filter(|text| !text.trim().is_empty());
        let unchanged = match (text, self.texts.get(&kind)) {
            (Some(new), Some(current)) => new == current,
            (None, None) => true,
            _ => false,
        };
        if unchanged {
            return;
        }
        if let Some(new) = text {
            self.texts.insert(kind, new.to_string());
        } else {
            self.texts.remove(&kind);
        }
        self.dirty = true;
    }

    /// Rebuild the rendered buffer if the reminder set changed since the last
    /// render; otherwise a no-op. Reuses the buffer's allocation.
    pub(crate) fn refresh(&mut self) {
        if !self.dirty {
            return;
        }
        self.rendered.clear();
        let mut entries: Vec<(&BlockKind, &String)> = self.texts.iter().collect();
        entries.sort_by(|a, b| {
            a.0.sort_key()
                .cmp(&b.0.sort_key())
                .then_with(|| a.0.cmp(b.0))
        });
        for (_, text) in entries {
            self.rendered.push(json!({
                "role": "user",
                "content": format!("<system-reminder>\n{text}\n</system-reminder>"),
            }));
        }
        self.dirty = false;
    }

    /// The rendered trailing messages for this request. Call
    /// [`refresh`](Self::refresh) earlier this tick so the buffer is current.
    pub(crate) fn as_slice(&self) -> &[Value] {
        &self.rendered
    }
}
