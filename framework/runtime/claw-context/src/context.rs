//! The agent's context: a self-caching, placement-keyed set of blocks plus an
//! ephemeral reminder tail, assembled into one LLM request.
//!
//! [`Context`] is the single home for an agent's request assembly. It owns the
//! content (one entry per [`BlockKind`]), a reused buffer holding the rendered
//! system prefix, and the [`Reminders`] tail. The agent **declares** blocks with
//! [`with`](Context::with) — call it every tick for anything that might change;
//! re-declaring identical content is a cheap no-op. [`request`](Context::request)
//! renders the prefix only when a block actually changed and hands back a
//! [`RequestContext`]. Ordering, change detection, caching, and reminder
//! rendering all live here; the caller never sorts, diffs, or tracks dirtiness.
//!
//! # Model: declare, don't mutate-on-change
//!
//! `with` is **incremental and safe**: a kind you do not declare keeps its last
//! value (it is never silently dropped), and empty content removes a kind. So an
//! agent declares its fixed blocks once (e.g. the instruction) and re-declares
//! the volatile ones every tick — never betting that it "remembers" to push a
//! change, and never re-rendering when nothing changed.

use std::borrow::Cow;
use std::collections::BTreeMap;

use serde_json::Value;

use crate::block::{Block, BlockKind};
use crate::reminder::Reminders;

/// Separator inserted between rendered blocks: a blank line keeps sections
/// visually distinct without editorializing block content.
const BLOCK_SEPARATOR: &str = "\n\n";

/// An agent's context: the placement-keyed blocks (one per [`BlockKind`]), the
/// cached system-prompt prefix, and the ephemeral reminder tail.
///
/// Declare content with [`with`](Self::with) (and [`reminder`](Self::reminder)),
/// then call [`request`](Self::request) to get the assembled request. The render
/// is dirty-gated by a content [`version`](Self::version), so a steady context
/// rebuilds nothing and reuses its buffer — re-declaring the full block set every
/// tick costs only a per-block equality check.
///
/// # Examples
///
/// ```
/// use claw_context::{Block, BlockKind, Context};
/// use serde_json::json;
///
/// let mut context = Context::new();
/// // Blocks may be declared in any order; the wire order is fixed by BlockKind.
/// context
///     .with(Block::new(BlockKind::AgentInstruction, "You are a helpful agent."))
///     .with(Block::new(BlockKind::OutputContract, "Answer in one concise paragraph."));
///
/// let history = json!([{ "role": "user", "content": "What's the weather?" }]);
/// let request = context.request(&history);
/// assert_eq!(
///     request.system(),
///     "You are a helpful agent.\n\nAnswer in one concise paragraph."
/// );
/// ```
pub struct Context {
    /// One owned content string per declared kind. Only ever holds non-absent
    /// content — empty content drops the key (see [`with`](Self::with)).
    blocks: BTreeMap<BlockKind, String>,
    /// Cached rendered system prefix. Rebuilt by [`request`](Self::request) only
    /// when `content_version != rendered_version`.
    rendered: String,
    /// Bumped on every real block change. The `version()` surface, and the gate
    /// for re-rendering.
    content_version: u64,
    /// The `content_version` the cached `rendered` reflects.
    rendered_version: u64,
    /// Ephemeral per-request reminders appended to the message tail; never
    /// persisted, and outside the cached system prefix.
    reminders: Reminders,
}

impl Default for Context {
    fn default() -> Self {
        Self::new()
    }
}

impl Context {
    /// An empty context: no blocks, no reminder.
    pub fn new() -> Self {
        Self {
            blocks: BTreeMap::new(),
            rendered: String::new(),
            content_version: 0,
            rendered_version: 0,
            reminders: Reminders::new(),
        }
    }

    /// Declare a context item. Blocks and reminders are stored in this context;
    /// message items belong to a [`ContextSink`] because they are per-request
    /// history contributions.
    pub fn with_item(&mut self, item: ContextItem<'_>) -> &mut Self {
        match item {
            ContextItem::Block(block) => self.with(block),
            ContextItem::Reminder { kind, text } => self.with_reminder(kind, text.as_deref()),
            ContextItem::Message { .. } => self,
        }
    }

    /// Declare a block (set or replace the content for its [`BlockKind`]).
    /// Chainable; meant to be called freely — re-declaring identical content is a
    /// no-op (no version bump, no re-render).
    ///
    /// - **Empty / whitespace-only content removes the kind** (it renders to
    ///   nothing); the map only ever holds visible blocks.
    /// - **A kind you do not declare keeps its last value** — declaration is
    ///   incremental and safe, never a silent drop.
    pub fn with(&mut self, block: Block<'_>) -> &mut Self {
        let Block { kind, content } = block;
        let is_empty = content.trim().is_empty();
        let changed = match self.blocks.get(&kind) {
            // The map never stores empty content, so any present entry differs
            // from empty; otherwise compare the actual strings.
            Some(existing) => existing.as_str() != content.as_ref(),
            None => !is_empty,
        };
        if changed {
            if is_empty {
                self.blocks.remove(&kind);
            } else {
                self.blocks.insert(kind, content.into_owned());
            }
            self.content_version = self.content_version.saturating_add(1);
        }
        self
    }

    /// Set (or clear with `None`) the tool reminder appended to the message tail.
    ///
    /// Kept for the common single-reminder path; use
    /// [`with_reminder`](Self::with_reminder) when the source has a more specific
    /// [`BlockKind`].
    pub fn reminder(&mut self, text: Option<&str>) -> &mut Self {
        self.with_reminder(BlockKind::ToolReminder, text)
    }

    /// Set (or clear with `None`) one ephemeral reminder kind appended to the
    /// message tail. Chainable. Dirty-gated inside the reminder channel, and
    /// outside the cached system prefix, so it never bumps [`version`](Self::version).
    pub fn with_reminder(&mut self, kind: BlockKind, text: Option<&str>) -> &mut Self {
        self.reminders.set(kind, text);
        self
    }

    /// Create a sink that accepts first-class context items for one request
    /// assembly pass.
    pub fn sink(&mut self) -> ContextSink<'_> {
        ContextSink::new(self)
    }

    /// The system-prefix version: a counter that advances only when a block's
    /// content actually changed. Stable across re-declarations of identical
    /// content, so it is a cheap, collision-free key for LLM prefix-cache
    /// stability. Reminder changes do not affect it (they are not in the prefix).
    pub fn version(&self) -> u64 {
        self.content_version
    }

    /// Assemble this request: pair the system prefix with the message tail
    /// (`history` plus the ephemeral reminders). Re-renders the prefix only when a
    /// block changed since the last call; otherwise reuses the cached string.
    pub fn request<'a>(&'a mut self, history: &'a Value) -> RequestContext<'a> {
        if self.rendered_version != self.content_version {
            self.rebuild();
            self.rendered_version = self.content_version;
        }
        self.reminders.refresh();
        RequestContext::new(&self.rendered, history, self.reminders.as_slice())
    }

    /// Re-render the cached prefix from the current blocks, in wire order
    /// (`band`, then `scope`, then in-band order), reusing the buffer.
    fn rebuild(&mut self) {
        let mut entries: Vec<(&BlockKind, &String)> = self.blocks.iter().collect();
        // Sort by the wire-order key; the full `BlockKind` Ord breaks ties between
        // custom blocks sharing a key, keeping the render deterministic.
        entries.sort_by(|a, b| {
            a.0.sort_key()
                .cmp(&b.0.sort_key())
                .then_with(|| a.0.cmp(b.0))
        });

        let mut buffer = std::mem::take(&mut self.rendered);
        buffer.clear();
        for (_, content) in entries {
            if !buffer.is_empty() {
                buffer.push_str(BLOCK_SEPARATOR);
            }
            buffer.push_str(content.trim());
        }
        self.rendered = buffer;
    }
}

/// One context contribution before it is rendered into its target request
/// channel.
#[derive(Debug, Clone)]
pub enum ContextItem<'a> {
    /// Stable or durable prose rendered into the cached system prefix.
    Block(Block<'a>),
    /// Structured chat history rendered into the request's messages segment.
    Message {
        /// The semantic kind used for cross-source ordering.
        kind: BlockKind,
        /// The message JSON object to append to history.
        value: &'a Value,
    },
    /// Ephemeral guidance rendered into the trailing reminder segment.
    Reminder {
        /// The semantic kind used for reminder ordering.
        kind: BlockKind,
        /// `None` clears the reminder kind.
        text: Option<Cow<'a, str>>,
    },
}

impl<'a> ContextItem<'a> {
    /// Construct a system-prefix block item.
    pub fn block(kind: BlockKind, content: impl Into<Cow<'a, str>>) -> Self {
        Self::Block(Block::new(kind, content))
    }

    /// Construct a structured history-message item.
    pub fn message(kind: BlockKind, value: &'a Value) -> Self {
        Self::Message { kind, value }
    }

    /// Construct an ephemeral reminder item.
    pub fn reminder(kind: BlockKind, text: impl Into<Cow<'a, str>>) -> Self {
        Self::Reminder {
            kind,
            text: Some(text.into()),
        }
    }

    /// Construct a reminder clear item.
    pub fn clear_reminder(kind: BlockKind) -> Self {
        Self::Reminder { kind, text: None }
    }
}

/// Request-local sink for context contributions.
///
/// Blocks and reminders are applied immediately to the owning [`Context`], which
/// keeps their caches and dirty gates. Message items are cloned into this sink's
/// request-local history buffer; this preserves the existing `Value::Array`
/// request shape without making adapters own the final history.
pub struct ContextSink<'a> {
    context: &'a mut Context,
    messages: Vec<(BlockKind, Value)>,
}

impl<'a> ContextSink<'a> {
    fn new(context: &'a mut Context) -> Self {
        Self {
            context,
            messages: Vec::new(),
        }
    }

    /// Accept one context item.
    pub fn item(&mut self, item: ContextItem<'_>) -> &mut Self {
        match item {
            ContextItem::Block(block) => {
                self.context.with(block);
            }
            ContextItem::Message { kind, value } => {
                self.message(kind, value);
            }
            ContextItem::Reminder { kind, text } => {
                self.context.with_reminder(kind, text.as_deref());
            }
        }
        self
    }

    /// Accept a system-prefix block.
    pub fn block(&mut self, block: Block<'_>) -> &mut Self {
        self.item(ContextItem::Block(block))
    }

    /// Accept a structured history message.
    pub fn message(&mut self, kind: BlockKind, value: &Value) -> &mut Self {
        self.messages.push((kind, value.clone()));
        self
    }

    /// Accept or clear an ephemeral reminder.
    pub fn reminder(&mut self, kind: BlockKind, text: Option<&str>) -> &mut Self {
        self.context.with_reminder(kind, text);
        self
    }

    /// Finish this sink and return the ordered history array for the request.
    pub fn into_history(mut self) -> Value {
        self.messages.sort_by(|a, b| {
            a.0.sort_key()
                .cmp(&b.0.sort_key())
                .then_with(|| a.0.cmp(&b.0))
        });
        Value::Array(
            self.messages
                .into_iter()
                .map(|(_, message)| message)
                .collect(),
        )
    }
}

/// The two wire fields of one LLM request, ready to feed to the API client: a
/// `system` prefix string and the `messages` tail. Produced only by
/// [`Context::request`]; nothing else assembles a request.
///
/// Every field is a **borrow**, so assembling per iteration allocates nothing:
/// `system` points into the context's reused prefix buffer, `history` into the
/// memory snapshot, and `reminders` into the context's reused reminder buffer.
/// The tail is a **two-segment view** — persisted `history` plus ephemeral
/// `reminders` — kept separate so appending a reminder never clones the
/// transcript; the backend iterates `history` then `reminders`.
#[derive(Clone, Copy, Debug)]
pub struct RequestContext<'a> {
    system: &'a str,
    history: &'a Value,
    reminders: &'a [Value],
}

impl<'a> RequestContext<'a> {
    /// Pair an assembled `system` prefix with the message tail's two segments.
    pub(crate) fn new(system: &'a str, history: &'a Value, reminders: &'a [Value]) -> Self {
        Self {
            system,
            history,
            reminders,
        }
    }

    /// The rendered system-prompt prefix.
    pub fn system(&self) -> &'a str {
        self.system
    }

    /// The persisted conversation history (a JSON array of messages).
    pub fn history(&self) -> &'a Value {
        self.history
    }

    /// The ephemeral trailing reminders (never persisted), in order.
    pub fn reminders(&self) -> &'a [Value] {
        self.reminders
    }
}
