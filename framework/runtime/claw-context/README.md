# claw-context

The agent's context assembler: the **single type** that turns declared content
into the two wire fields of an LLM request — the `system` prefix string and the
`messages` tail.

This crate owns **placement, change detection, and rendering**, never
**content**. A `Context` orders the blocks you declare into the mutability-graded
wire layout (see `docs/context-model.md`), drops absent ones, caches the rendered
prefix, and pairs it with the tail. What each block *contains* — instructions,
persona, memory, retrieved knowledge — is supplied by callers.

## The two lanes

A request has exactly two wire fields, and `Context` produces both. Providers may
feed it directly with `Block`s or through a `ContextSink` that accepts
first-class `ContextItem`s (`Block`, history `Message`, or `Reminder`):

- **PREFIX (cacheable) -> `system`**: the declared `Block`s rendered into one
  string, in wire order. Held in a **reused buffer** and re-rendered only when a
  block actually changes, so a steady prefix costs nothing per iteration.
- **TAIL -> `messages`**: the persisted conversation `history` (owned by memory)
  plus ephemeral `reminders` (per-request nudges, never persisted). History is
  passed in by reference; reminders are owned and dirty-gated by `Context`.

`Context::request(history)` pairs the prefix with the tail's two segments as a
`RequestContext` of all-borrows (`history` + `reminders` stay separate so
appending a reminder never clones the transcript) — the single hand-off fed
straight into the API client.

## How it works

You **declare** blocks with `with(Block)` in any order; `Context` is the sole
authority on wire order, sorting by **band**, then **scope**, then in-band order.
Declaration is incremental and safe:

- A kind you don't declare **keeps its last value** (never a silent drop).
- **Empty content removes a kind** (it renders to nothing).
- **Re-declaring identical content is a free no-op** — call `with` every tick for
  anything that might change; the context is never stale, and it re-renders only
  on a real change (tracked by `version()`).

Because blocks are keyed by `BlockKind`, a "duplicate canonical block" is
**unrepresentable** — there is no build error to handle, and `request` never
fails.

| Item | Role |
|---|---|
| `Context` | The owned, self-caching context: declare with `with(Block)` / `with_reminder(kind, text)`, or create a `sink()` for mixed `ContextItem`s; read with `request(&history)`. `version()` advances only on a real prefix change (a cheap LLM prefix-cache key). |
| `ContextItem<'a>` / `ContextSink<'a>` | The unified contribution path for adapters: block prose, history messages, and typed reminders enter through one sink while `Context` keeps placement and caches. |
| `Block<'a>` | One piece of content plus its placement: `Block::new(kind, text)`. Content is a `Cow`, so callers pass `&str`/`String`/`Cow` freely; `Context` copies it on a real change. |
| `BlockKind` | The canonical context kinds (e.g. `Soul`, `AssistantIdentity`, `UserProfile`, `GlobalMemory`, `AgentInstruction`, `ToolPolicy`, `ToolReminder`, `AgentMemory`, `SkillList`, `ModeFraming`, `ReasoningEffort`, `RecentContext`, `OutputContract`) plus `Custom { band, scope, order, label }` for caller-defined blocks. |
| `Band` / `Scope` | The two axes the layout sorts on (durability band, ownership scope). |
| `RequestContext<'a>` | The assembled `(system, messages)` hand-off: a `system` prefix plus the tail's two segments (`history` + `reminders`), all borrows. |

## Example

```rust
use claw_context::{Block, BlockKind, Context};
use serde_json::json;

let mut context = Context::new();
context
    .with(Block::new(BlockKind::AgentInstruction, "You are a helpful agent."))
    .with(Block::new(BlockKind::OutputContract, "Answer in one concise paragraph."));

let history = json!([{ "role": "user", "content": "What's the weather?" }]);
let request = context.request(&history);
assert_eq!(
    request.system(),
    "You are a helpful agent.\n\nAnswer in one concise paragraph.",
);
```

A fuller, runnable walkthrough — a working-mode context, a `Custom` block, a
reminder, and the `version()` change-detection — is in `examples/`:

```bash
cargo run -p claw-context --example build_context --target x86_64-unknown-linux-gnu
```

## Where it fits

Pure-Rust, depends only on `serde_json` (to carry the structured `messages`
tail). It bundles into the firmware's `claw_rt` staticlib and is fully
host-testable. Content providers (instruction loaders, memory providers, skill
sets, summarizers) live in other crates and feed their prose/items in via
`with` or `sink`; the conversation `history` is passed in by the agent.
