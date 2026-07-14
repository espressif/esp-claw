//! A minimal, platform-agnostic `tracing` [`Subscriber`] that tracks the
//! *current thread's* active span stack and writes one line per event /
//! span-boundary to an injected [`TraceSink`], tagging each with its
//! `span`/`parent` ids and logical `task` so an offline tool rebuilds the tree
//! and the in-effect context.
//!
//! This is pure Rust (only `tracing`'s re-exported core traits): no
//! `tracing-subscriber` (so no `sharded-slab`/`regex`), and no platform/FFI. The
//! actual output target is a [`TraceSink`] supplied by the caller — this crate's
//! [`init_tracing`](crate::init_tracing) wires it to `claw_sys`'s `ESP_LOGx`
//! sink; tests inject a capturing one.
//!
//! ## What it solves
//!
//! The `log` bridge (`tracing/log-always`) drops span context: a child event
//! prints none of its ancestors' ids, and there is no stable per-span instance
//! id, so a flat log cannot be reassembled into a tree — and concurrent sessions
//! interleave irrecoverably. This subscriber fixes both **without** plumbing ids
//! through every layer:
//!
//! - Span context propagates automatically via the active span stack (the whole
//!   point of `tracing` spans), so call sites pass no extra arguments.
//! - A span may open a logical async task with the reserved `trace.task` field.
//!   Its descendants inherit that label even when the future migrates between
//!   executor threads, so concurrent drives stay separable. Spans outside a
//!   logical task fall back to the OS thread / FreeRTOS task label.
//! - Span lifetimes are emitted as `enter` / `exit` lines keyed by a
//!   process-unique `span=` id with a `parent=` edge, so an offline tool rebuilds
//!   the tree from `(span, parent)` and computes per-span timing by differencing
//!   the sink's timestamps for the matching `enter`/`exit` lines.
//!
//! ## Line grammar (for the offline visualizer)
//!
//! Each line is `TRACE <timestamp> <type> <tracing-context> <incremental-context>*
//! <custom-context>`, where the structural blocks are angle-bracketed and their
//! `key=value` tokens never contain spaces; everything after the blocks is the
//! free-form custom context. See `docs/trace-format.md` for the authoritative
//! specification — this module implements it.
//!
//! ```text
//! TRACE <ts> enter <span=<id> parent=<id|none> task=<label> span-name=<name> target=<module>> <context=<group> <key>=<value> …>* <custom…>
//! TRACE <ts> exit  <span=<id> task=<label>>
//! TRACE <ts> event <span=<id|none> task=<label> event-name=<name> target=<module>> <custom…>
//! ```
//!
//! - `<ts>` is a monotonic millisecond clock; a span's duration is the difference
//!   between its `exit` and `enter` timestamps.
//! - `span`/`parent` are framework-assigned, monotonic, never-recycled ids. An
//!   `event` outside any span has `span=none`; a root span has `parent=none`.
//! - `enter`/`exit` are emitted once per span lifetime (span creation / close),
//!   not per poll; the per-thread span stack is maintained on the `tracing`
//!   enter/exit callbacks so events resolve their enclosing span.
//! - Incremental context is grouped: a span field named `group.key` whose `group`
//!   is a registered [`ContextGroup`] is rendered once, on the `enter` line of the
//!   span that opens it, as a `<context=<group> …>` block. A span that opens a
//!   logical task repeats the complete inherited context so that task's stream
//!   is independently replayable; other descendants emit only their delta.
//!   Events do not repeat context. Any other field is custom context.

use std::cell::RefCell;
use std::collections::HashMap;
use std::fmt::{self, Write as _};
use std::sync::{Arc, Mutex};
use std::time::Instant;

// `tracing` re-exports the core traits/types this subscriber needs, so no
// separate `tracing-core` dependency is required.
use tracing::field::{Field, Visit};
use tracing::span::{Attributes, Id, Record};
use tracing::{Event, Level, Metadata, Subscriber};

/// Where a [`FlatTreeSubscriber`] writes its formatted lines. Implemented by the
/// platform sink wiring (this crate's `ClawTraceSink`, which writes through
/// `claw_sys`'s `ESP_LOGx` sink) and by tests (a capturing sink), keeping this
/// module free of any output/FFI concern.
pub trait TraceSink: Send + Sync {
    /// Write one already-formatted, single-line record at `level`, tagged `tag`
    /// (the record's `target`/module path). The line never contains a newline.
    fn write_line(&self, level: Level, tag: &str, line: &str);
}

/// A named, ordered set of inherited-context keys (see [`with_context_group_keys`]).
///
/// `claw-log` itself bakes in **no** group; the caller registers each one at
/// subscriber init (e.g. `claw_core` registers `"run"` with
/// `["system", "session", "turn", "agent", "iteration"]`). A span field named
/// `group.key` whose `group` matches `name` and whose `key` is in `keys` becomes
/// this group's incremental context.
///
/// [`with_context_group_keys`]: FlatTreeSubscriber::with_context_group_keys
struct ContextGroup {
    /// The group prefix used at call sites (`group.key`) and in the rendered
    /// `<context=<name> …>` block header.
    name: &'static str,
    /// The group's closed, ordered key set. A `group.key` field whose key is not
    /// here is a typo (`debug_assert!`).
    keys: Vec<&'static str>,
}

/// Per-group inherited context: the outer `Vec` is indexed parallel to the
/// subscriber's registered [`ContextGroup`]s; each inner `Vec` holds the
/// `(key, value)` entries in effect for that group.
type GroupedContext = Vec<Vec<(&'static str, String)>>;

/// Reserved span field that opens a logical async task/lane. It is consumed by
/// the subscriber and therefore never appears as custom context.
const LOGICAL_TASK_FIELD: &str = "trace.task";

/// Immutable per-span data captured at creation and reused on exit/descendants.
struct SpanRecord {
    /// The span's `target` (module path), used as the sink tag on the exit line.
    target: &'static str,
    /// The span's level (forwarded to the sink for the exit line).
    level: Level,
    /// Inherited context (ancestor + own), per group, propagated to descendants.
    context: GroupedContext,
    /// Logical async task inherited by descendants and used for this span's
    /// lifetime records. This must not be recomputed at close because a future
    /// may resume or be dropped on a different executor thread.
    task: String,
}

/// Merge a child's own per-group context onto its inherited (ancestor) context:
/// keep the ancestor entries, then upsert each of the child's (child value wins),
/// group by group.
fn merge_grouped_context(ancestor: GroupedContext, own: GroupedContext) -> GroupedContext {
    ancestor
        .into_iter()
        .enumerate()
        .map(|(group_index, ancestor_bucket)| {
            let own_bucket = match own.get(group_index) {
                Some(bucket) => bucket.clone(),
                None => Vec::new(),
            };
            merge_bucket(ancestor_bucket, own_bucket)
        })
        .collect()
}

/// Upsert one group's `(key, value)` entries (child value wins).
fn merge_bucket(
    ancestor: Vec<(&'static str, String)>,
    own: Vec<(&'static str, String)>,
) -> Vec<(&'static str, String)> {
    let mut merged = ancestor;
    for (key, value) in own {
        match merged.iter_mut().find(|(existing, _)| *existing == key) {
            Some(slot) => slot.1 = value,
            None => merged.push((key, value)),
        }
    }
    merged
}

/// Render one group's *opened* delta — entries in `full` not already in effect in
/// `ancestor` (same key and value) — as a leading-space `" key=value"` segment.
/// Empty when this span opens nothing new for the group.
fn render_opened_bucket(
    ancestor: &[(&'static str, String)],
    full: &[(&'static str, String)],
) -> String {
    let mut segment = String::new();
    for (key, value) in full {
        let already_in_effect = ancestor
            .iter()
            .any(|(open_key, open_value)| open_key == key && open_value == value);
        if !already_in_effect {
            let _ = write!(segment, " {key}={value}");
        }
    }
    segment
}

/// Render `none` for the absent-id sentinel (`0`), else the id itself. Used for
/// `span=`/`parent=` where `0` means "no span" / "no parent".
fn id_or_none(id: u64) -> String {
    if id == 0 {
        "none".to_string()
    } else {
        id.to_string()
    }
}

fn raw_id(id: &Id) -> u64 {
    id.into_u64()
}

/// Refcounted slot in the global span map (`tracing` may `clone_span`).
struct SpanSlot {
    record: Arc<SpanRecord>,
    refs: u64,
}

thread_local! {
    /// The active span stack for *this* thread: `(id, record)` pushed on enter,
    /// popped on exit. The hot event path only reads the top, so it never locks
    /// the global map.
    static STACK: RefCell<Vec<(u64, Arc<SpanRecord>)>> = const { RefCell::new(Vec::new()) };

    /// This physical thread's fallback label, computed once (thread name, else
    /// `ThreadId(n)`). Logical async tasks do not use this label.
    static PHYSICAL_TASK: String = physical_task_label();
}

fn physical_task_label() -> String {
    let current = std::thread::current();
    if let Some(name) = current.name() {
        let label = sanitize_token(name);
        if !label.is_empty() {
            return label;
        }
    }
    // `ThreadId`'s Debug is `ThreadId(n)` — no spaces, safe as a token.
    format!("{:?}", current.id())
}

/// Collects a span/event's fields, routing each `group.key` field to its
/// registered [`ContextGroup`] bucket and the rest into a space-joined custom
/// `key=value` string; captures the special `message` field separately (events
/// carry their text there).
struct FieldVisitor<'groups> {
    /// The registered groups (empty for events, which open no context).
    groups: &'groups [ContextGroup],
    /// Space-joined `key=value` custom fields (no spaces inside a token).
    fields: String,
    /// The implicit `message` field (an event's free text), if present.
    message: Option<String>,
    /// Per-group incremental context, indexed parallel to `groups`.
    context: GroupedContext,
    /// Explicit logical task opened by a span's reserved `trace.task` field.
    logical_task: Option<String>,
    /// Events use the same visitor but do not interpret `trace.task` as a
    /// structural field.
    capture_logical_task: bool,
}

impl<'groups> FieldVisitor<'groups> {
    fn new(groups: &'groups [ContextGroup], capture_logical_task: bool) -> Self {
        Self {
            groups,
            fields: String::new(),
            message: None,
            context: groups.iter().map(|_| Vec::new()).collect(),
            logical_task: None,
            capture_logical_task,
        }
    }

    fn push(&mut self, name: &'static str, value: fmt::Arguments<'_>) {
        if name == "message" {
            self.message = Some(format!("{value}"));
            return;
        }
        if self.capture_logical_task && name == LOGICAL_TASK_FIELD {
            self.logical_task = Some(format!("{value}"));
            return;
        }
        // A `group.key` field routes to that group's incremental-context bucket;
        // everything else is custom context.
        if let Some((group_index, key)) = self.route(name) {
            if let Some(bucket) = self.context.get_mut(group_index) {
                bucket.push((key, format!("{value}")));
            }
            return;
        }
        if !self.fields.is_empty() {
            self.fields.push(' ');
        }
        self.fields.push_str(name);
        self.fields.push('=');
        let _ = write!(self.fields, "{value}");
    }

    /// Resolve a `group.key` field name to its `(group_index, key)`. Returns
    /// `None` (→ custom context) when the name is not dotted or its prefix is not
    /// a registered group. A registered prefix with a key outside the group's
    /// closed set is a typo and trips `debug_assert!`.
    fn route(&self, name: &'static str) -> Option<(usize, &'static str)> {
        let (group_name, key) = name.split_once('.')?;
        let group_index = self
            .groups
            .iter()
            .position(|group| group.name == group_name)?;
        let group = self.groups.get(group_index)?;
        match group.keys.iter().copied().find(|declared| *declared == key) {
            Some(declared) => Some((group_index, declared)),
            None => {
                debug_assert!(
                    false,
                    "trace: field '{name}' uses registered group '{group_name}' but key '{key}' is not in its closed set"
                );
                None
            }
        }
    }
}

impl Visit for FieldVisitor<'_> {
    fn record_str(&mut self, field: &Field, value: &str) {
        self.push(field.name(), format_args!("{value}"));
    }

    fn record_bool(&mut self, field: &Field, value: bool) {
        self.push(field.name(), format_args!("{value}"));
    }

    fn record_u64(&mut self, field: &Field, value: u64) {
        self.push(field.name(), format_args!("{value}"));
    }

    fn record_i64(&mut self, field: &Field, value: i64) {
        self.push(field.name(), format_args!("{value}"));
    }

    fn record_f64(&mut self, field: &Field, value: f64) {
        self.push(field.name(), format_args!("{value}"));
    }

    fn record_debug(&mut self, field: &Field, value: &dyn fmt::Debug) {
        // `%x` (Display) and `?x` (Debug) span/event values land here.
        self.push(field.name(), format_args!("{value:?}"));
    }
}

/// Replace newlines (which would split a single log line) with spaces. Applied to
/// the whole rendered line, so the free-form custom context may keep its spaces.
fn sanitize_line(input: &str) -> String {
    input.replace(['\n', '\r'], " ")
}

/// Collapse whitespace to `_` so a value is a single space-free token. Used for
/// the framework-derived `event-name` (whose default is `event <file>:<line>`).
fn sanitize_token(input: &str) -> String {
    input.replace([' ', '\t', '\n', '\r'], "_")
}

/// A `tracing` subscriber that emits the flat-tree line grammar to `sink`.
///
/// Generic over the sink so it stays unit-testable (inject a capturing sink) and
/// platform-agnostic (the caller supplies `ESP_LOGx`/stderr/UART/file output).
pub struct FlatTreeSubscriber<S: TraceSink> {
    /// Anchor for the monotonic `<ts>` column (ms since this subscriber was built).
    start: Instant,
    /// Monotonic span id allocator. Use a mutex, not `AtomicU64`: ESP targets do
    /// not consistently provide native 64-bit atomics.
    next_id: Mutex<u64>,
    spans: Mutex<HashMap<u64, SpanSlot>>,
    sink: S,
    /// Target-prefix allowlist. Empty means "accept every target"; otherwise a
    /// span/event is kept only if its `target` starts with one of these prefixes.
    /// Used to keep third-party library noise (reqwest/hyper/h2/rustls/…) out of
    /// the trace — see [`with_allowed_target_prefix`](Self::with_allowed_target_prefix).
    allowed_prefixes: Vec<String>,
    /// Inherited-context groups, in registration order. Empty = no incremental
    /// context (every field is custom) — see [`with_context_group_keys`].
    ///
    /// [`with_context_group_keys`]: Self::with_context_group_keys
    context_groups: Vec<ContextGroup>,
}

impl<S: TraceSink> FlatTreeSubscriber<S> {
    /// Build a subscriber writing to `sink`. With no target allowlist it accepts
    /// every target ([`with_allowed_target_prefix`]) and with no context group it
    /// treats every field as custom ([`with_context_group_keys`]).
    ///
    /// [`with_allowed_target_prefix`]: Self::with_allowed_target_prefix
    /// [`with_context_group_keys`]: Self::with_context_group_keys
    pub fn with_sink(sink: S) -> Self {
        Self {
            start: Instant::now(),
            // `span::Id` must be non-zero; start at 1 and reserve 0 for "no span".
            next_id: Mutex::new(1),
            spans: Mutex::new(HashMap::new()),
            sink,
            allowed_prefixes: Vec::new(),
            context_groups: Vec::new(),
        }
    }

    /// Only emit spans/events whose `target` (module path) starts with `prefix`.
    ///
    /// Call once per allowed prefix. The intended use is `"claw"` so only this
    /// firmware's own crates are traced and noisy dependency `tracing` output
    /// (reqwest/hyper/h2/…) is dropped. Because filtering is by static target, a
    /// rejected callsite is cached as [`Interest::never`](tracing::subscriber::Interest)
    /// by `tracing`'s default `register_callsite`, so it costs nothing after the
    /// first check.
    pub fn with_allowed_target_prefix(mut self, prefix: impl Into<String>) -> Self {
        self.allowed_prefixes.push(prefix.into());
        self
    }

    /// Register an inherited-context group `name` with its closed, ordered `keys`.
    ///
    /// A span field named `name.<key>` then becomes this group's incremental
    /// context, rendered once (as a `<context=<name> …>` block) on the `enter`
    /// line of the span that opens it. `claw-log` bakes in no group; the caller
    /// declares them — e.g. `claw_core` registers `"run"` with
    /// `["system", "session", "turn", "agent", "iteration"]`.
    pub fn with_context_group_keys(
        mut self,
        name: &'static str,
        keys: impl IntoIterator<Item = &'static str>,
    ) -> Self {
        debug_assert!(
            !self.context_groups.iter().any(|group| group.name == name),
            "trace: context group '{name}' registered more than once"
        );
        self.context_groups.push(ContextGroup {
            name,
            keys: keys.into_iter().collect(),
        });
        self
    }

    /// True when `target` passes the allowlist (or no allowlist is set).
    fn target_allowed(&self, target: &str) -> bool {
        self.allowed_prefixes.is_empty()
            || self
                .allowed_prefixes
                .iter()
                .any(|prefix| target.starts_with(prefix.as_str()))
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, HashMap<u64, SpanSlot>> {
        self.spans
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn next_span_id(&self) -> u64 {
        let mut next_id = self
            .next_id
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let id = *next_id;
        *next_id = next_id.saturating_add(1);
        id
    }

    /// Milliseconds since the subscriber was built (the `<ts>` column).
    fn now_ms(&self) -> u64 {
        u64::try_from(self.start.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    /// Pad a context to one bucket per registered group (an empty parent stack
    /// yields no buckets), so merge/render can index group-by-group.
    fn normalize_context(&self, mut context: GroupedContext) -> GroupedContext {
        while context.len() < self.context_groups.len() {
            context.push(Vec::new());
        }
        context
    }

    /// Render the `<context=<group> …>` blocks this span opens (the per-group
    /// delta of `full` over `ancestor`); groups that open nothing emit no block.
    fn render_opened_groups(&self, ancestor: &GroupedContext, full: &GroupedContext) -> String {
        let mut blocks = String::new();
        for (group_index, group) in self.context_groups.iter().enumerate() {
            let ancestor_bucket = match ancestor.get(group_index) {
                Some(bucket) => bucket.as_slice(),
                None => &[],
            };
            let full_bucket = match full.get(group_index) {
                Some(bucket) => bucket.as_slice(),
                None => &[],
            };
            let delta = render_opened_bucket(ancestor_bucket, full_bucket);
            if !delta.is_empty() {
                let _ = write!(blocks, " <context={name}{delta}>", name = group.name);
            }
        }
        blocks
    }

    fn write(&self, level: Level, tag: &str, line: &str) {
        self.sink.write_line(level, tag, &sanitize_line(line));
    }
}

impl<S: TraceSink + 'static> Subscriber for FlatTreeSubscriber<S> {
    fn enabled(&self, metadata: &Metadata<'_>) -> bool {
        // Target allowlist keeps dependency `tracing` noise (reqwest/hyper/h2/…)
        // out of the trace. Level filtering is left to the compile-time
        // `tracing` `max_level_*` features and the sink's runtime ceiling.
        // `tracing`'s default `register_callsite` caches this per callsite, so a
        // rejected target becomes `Interest::never` and is never re-checked.
        self.target_allowed(metadata.target())
    }

    fn new_span(&self, attributes: &Attributes<'_>) -> Id {
        let metadata = attributes.metadata();
        let mut visitor = FieldVisitor::new(&self.context_groups, true);
        attributes.record(&mut visitor);

        // The structural parent, inherited context, and inherited logical task
        // come from the span currently entered during this future's poll.
        let (parent_id, parent_context, parent_task) =
            STACK.with(|stack| match stack.borrow().last() {
                Some((id, record)) => (*id, record.context.clone(), Some(record.task.clone())),
                None => (0, GroupedContext::new(), None),
            });
        let parent_context = self.normalize_context(parent_context);
        let context = merge_grouped_context(parent_context.clone(), visitor.context);
        let opened_logical_task = visitor
            .logical_task
            .as_deref()
            .map(sanitize_token)
            .filter(|task| !task.is_empty());
        let opens_logical_task = opened_logical_task.is_some();
        let task = match opened_logical_task {
            Some(task) => task,
            None => match parent_task {
                Some(task) => task,
                None => PHYSICAL_TASK.with(Clone::clone),
            },
        };

        let id = self.next_span_id();

        // `enter` line == span creation (one pair per span; not per poll). The
        // span is pushed onto the stack later, in the `enter` callback. A new
        // logical task has no prior consumer-side stack, so seed its lane with
        // the complete effective context; same-task descendants stay compact.
        let context_base = if opens_logical_task {
            GroupedContext::new()
        } else {
            parent_context
        };
        let opened = self.render_opened_groups(&context_base, &context);
        let timestamp = self.now_ms();
        let mut line = format!(
            "TRACE {timestamp} enter <span={id} parent={parent} task={task} span-name={name} target={target}>",
            parent = id_or_none(parent_id),
            name = metadata.name(),
            target = metadata.target(),
        );
        line.push_str(&opened);
        if !visitor.fields.is_empty() {
            line.push(' ');
            line.push_str(&visitor.fields);
        }
        self.write(*metadata.level(), metadata.target(), &line);

        let record = Arc::new(SpanRecord {
            target: metadata.target(),
            level: *metadata.level(),
            context,
            task,
        });
        self.lock().insert(id, SpanSlot { record, refs: 1 });
        Id::from_u64(id)
    }

    fn record(&self, _span: &Id, _values: &Record<'_>) {
        // No-op: this firmware supplies every span field at creation (captured in
        // `new_span`, where the `enter` line is emitted), so late `Span::record`
        // has nothing to add. Add merge-back here if dynamic fields are adopted.
    }

    fn record_follows_from(&self, _span: &Id, _follows: &Id) {}

    fn event(&self, event: &Event<'_>) {
        let metadata = event.metadata();
        // Events open no incremental context, so route nothing to groups: every
        // field (and the message) is custom context.
        let mut visitor = FieldVisitor::new(&[], false);
        event.record(&mut visitor);

        let timestamp = self.now_ms();
        let event_name = sanitize_token(metadata.name());

        let mut custom = visitor.fields;
        if let Some(message) = visitor.message {
            if !custom.is_empty() {
                custom.push(' ');
            }
            custom.push_str(&message);
        }

        let render_line = |span_id, task: &str| {
            let mut line = format!(
                "TRACE {timestamp} event <span={span} task={task} event-name={event_name} target={target}>",
                span = id_or_none(span_id),
                target = metadata.target(),
            );
            if !custom.is_empty() {
                line.push(' ');
                line.push_str(&custom);
            }
            line
        };
        let line = STACK.with(|stack| match stack.borrow().last() {
            Some((id, record)) => render_line(*id, &record.task),
            None => PHYSICAL_TASK.with(|task| render_line(0, task)),
        });
        self.write(*metadata.level(), metadata.target(), &line);
    }

    fn enter(&self, span: &Id) {
        // No line here: the `enter` line is emitted at creation. This only
        // maintains the per-thread stack so events resolve their enclosing span
        // and children inherit context.
        let id = raw_id(span);
        let record = self.lock().get(&id).map(|slot| slot.record.clone());
        if let Some(record) = record {
            STACK.with(|stack| stack.borrow_mut().push((id, record)));
        }
    }

    fn exit(&self, span: &Id) {
        // No line here: the `exit` line is emitted at close. This only pops the
        // per-thread stack. Spans are entered/exited LIFO per thread (RAII
        // guards), so the top is this span; guard against a mismatch just in case.
        let id = raw_id(span);
        STACK.with(|stack| {
            let mut stack = stack.borrow_mut();
            if matches!(stack.last(), Some((top, _)) if *top == id) {
                stack.pop();
            }
        });
    }

    fn clone_span(&self, id: &Id) -> Id {
        let raw = raw_id(id);
        if let Some(slot) = self.lock().get_mut(&raw) {
            slot.refs = slot.refs.saturating_add(1);
        }
        id.clone()
    }

    fn try_close(&self, id: Id) -> bool {
        let raw = raw_id(&id);
        // Drop the last reference under the lock; emit the `exit` line (span
        // destruction == one pair per span) only when the span is actually gone.
        let closed = {
            let mut map = self.lock();
            let Some(slot) = map.get_mut(&raw) else {
                return false;
            };
            slot.refs = slot.refs.saturating_sub(1);
            if slot.refs > 0 {
                None
            } else {
                map.remove(&raw)
            }
        };
        match closed {
            Some(slot) => {
                let timestamp = self.now_ms();
                let line = format!(
                    "TRACE {timestamp} exit <span={raw} task={task}>",
                    task = slot.record.task
                );
                self.write(slot.record.level, slot.record.target, &line);
                true
            }
            None => false,
        }
    }
}
