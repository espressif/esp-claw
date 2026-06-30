# claw-utils

Small, dependency-light helpers shared across the claw Rust crates. Two things
live here, both used widely enough to deserve a single home rather than being
copied per crate:

1. **`TruncatedText`** — log-safe text truncation.
2. **`define_prefixed_id!`** — the strongly typed, wire-prefixed id newtype macro.

The crate name is `claw-utils`; the library is imported as `claw_utils`.

## `TruncatedText<T>` — log-safe truncation

A `Display` wrapper that renders at most `limit` bytes of text, always backing
off to a UTF-8 char boundary, and appends `"..."` when it had to cut. It never
allocates and never panics on multi-byte input.

```rust
use claw_utils::TruncatedText;

// Platform default ceiling: compact on device, unbounded on host.
log::debug!("payload = {}", TruncatedText::new(&body));

// Explicit, testable ceiling.
let s = TruncatedText::with_limit(&body, 96).to_string();
```

The default limit is platform-aware: on the `espidf` target it caps lines at 96
bytes to save flash and UART bandwidth; on the host it is `usize::MAX` (a no-op)
so the CLI and offline tooling see the full text. Use `with_limit` to override
at a call site.

## `define_prefixed_id!` — wire-prefixed id newtypes

Defines a `usize` newtype whose wire form carries a fixed string prefix
(`session-1`, `task-2`, …). This gives each domain id its own type — a `TaskId`
can't be passed where a `SessionId` is expected — while keeping a compact,
human-readable serialized form.

```rust
use claw_utils::define_prefixed_id;

define_prefixed_id!(SessionId, "session-", "session");
define_prefixed_id!(TaskId, "task-", "task");

let id = SessionId::new(1);
assert_eq!(id.to_wire(), "session-1");
assert_eq!(SessionId::from_wire("session-1").unwrap(), id);
```

Each generated type derives `Clone, Copy, Debug, PartialEq, Eq, Hash` and
implements:

- `new(usize)` / `From<usize>` — construct from the raw number.
- `to_wire()` / `Display` — render to the prefixed string.
- `from_wire(&str)` / `FromStr` — parse and validate the prefix.
- `Serialize` / `Deserialize` — (de)serialize **by the wire string**, so the
  prefix is part of the on-wire representation.

Parsing returns `IdParseError` (`Empty` or `Invalid { kind, value }`) when the
input is blank or lacks the expected prefix and numeric suffix. `parse_prefixed_id`
is exposed as the shared parsing primitive the macro builds on.

## Where it fits

`claw-utils` has no platform dependencies (only `thiserror`), so it compiles and
tests identically on device and host. Higher-level crates such as `claw_core`
build their domain ids (`IterationId`, `TaskId`, `StepId`, `WorkerId`,
`SessionId`) with `define_prefixed_id!` and use `TruncatedText` whenever
untrusted or large strings reach a log or trace line.
