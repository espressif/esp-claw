# claw-memory

The agent memory subsystem.

Three independent pieces live here: the **`TranscriptStore`** — a pure,
append-only verbatim record of a conversation's turns — **`ProfileStore`** for
editable global profile documents (`soul.md`, `identity.md`, `user.md`), and
**long-term memory** (durable facts). These stores know nothing about prompt
assembly, summarization, token budgets, or agent tools. Assembling an LLM
context window is the *agent layer's* job, built on top of the stores via
context adapters in `claw-core`.

The crate only defines the `Compactor` **seam** — the contract for folding an
aged window of messages into a shorter summary. It carries no LLM dependency;
the ready-made LLM-backed compactor (`LlmCompactor`) and the rolling-summary
adapter that drives it both live in `claw_core` (the layer that owns the LLM
client). The store is never asked to compact.

As a core crate it depends only on the `claw-interface` `ClawFs` persistence
seam, never on the platform boundary (`claw-sys`). The concrete filesystem is
selected by the store type parameter (device firmware uses its real FS type;
host CLIs and tests use `claw_interface::MemFs` / `DiskFs`), so the crate is
fully host-testable.

## Public API

| Item | Role |
|---|---|
| `TranscriptStore` | The per-conversation verbatim transcript. `TranscriptStore::<F>::new(id, config)`, `group()`, `messages()`, `turns_snapshot()`, `open_turn_messages()`, `version()`, `flush()`. Pure append-only — never compacts. |
| `TranscriptConfig` | Tuning: `dir`, `persist_debounce`. Build with `TranscriptConfig::new(dir)`. |
| `Turn` / `TurnId` | A committed turn (`id` + `messages`) and its monotonic logical id, exposed by `turns_snapshot()` so adapters can read committed turns. |
| `GroupGuard` | One turn, returned by `group()`. `append_user`, `append_assistant`, `append_tool_result`, `append_patch`. Commits the whole turn as one record on drop. |
| `Compactor` / `CompactError` | The summarization seam: fold an aged message window into a shorter summary. Driven by the agent layer, **not** the store. |
| `ProfileStore` and friends | Editable global profile documents: `Soul`, assistant identity, and user profile. Pure whole-file storage over `ClawFs`; projected into context by `claw-core`. |
| `LongTermMemory` and friends | Durable per-agent / global fact storage. |
| `NoopCompactor` | *(feature `compactor-stub`)* A never-compacts stub for host CLIs and tests. |

### How a turn flows

1. Call `store.group()` to open a turn; append user / assistant / tool-result
   messages to the returned `GroupGuard`.
2. When the guard drops, the whole turn is committed as a single record and
   `version()` advances.
3. `store.messages()` returns the full verbatim transcript (committed turns plus
   the open one) — no summaries spliced in; the store keeps everything.
4. `store.flush()` forces a checkpoint (e.g. on clean shutdown).

**Compaction is not the store's concern.** In `claw-core`, a
`RollingSummaryContextAdapter` reads aged turns via `turns_snapshot()`,
summarizes them through an injected `Compactor`, and a
`RecentMessagesContextAdapter` renders the verbatim tail. The two coordinate
through a shared cursor marking the boundary between the summarized prefix and
the verbatim tail. Bounding on-disk growth (retention) is likewise a separate,
future concern — not the store's.

## Features

| Feature | Default | Effect |
|---|---|---|
| `compactor-stub` | no | Adds `NoopCompactor` (host-only convenience). |

## Example

```bash
cargo run -p claw-memory --example conversation --target x86_64-unknown-linux-gnu
```

Drives a `TranscriptStore` through a few turns over an in-memory `MemFs`, then
prints the verbatim message list the model would receive. See the crate-level
rustdoc for the same flow with inline commentary.

## Where it fits

A pure-Rust core crate (no platform/FFI). It persists through the injected
`ClawFs`, so it is fully host-testable; the tests under `tests/` exercise it
over both in-memory and on-disk `ClawFs` doubles.
