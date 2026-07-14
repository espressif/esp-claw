# Rust

Rust Best Practices by Finn(Ziheng) Sheng.

- Prefer returning errors via `Result` over panicking. Only panic for truly unreachable/buggy states.
- Use `unimplemented!()` for code paths that are intentionally not implemented and will never be (e.g., unsupported enum variants that should never occur).
- Use `todo!()` for stubs that are meant to be implemented in the future (clear marker for "work in progress").
- Do NOT use `unimplemented!()` or `todo!()` in places where a proper error variant can be returned via `Result`. Panics are for bugs, not expected error conditions.

## Assertions (`assert!`, `assert_eq!`, etc.)

- Use `assert!` only for conditions that **logically cannot occur**, but **programmatically you cannot guarantee**. For example: "Memory usage will never exceed 512 bytes in this design, but the addition operation could theoretically overflow" — use `assert!` to document and verify this assumption.
- Do NOT use `assert!` for expected runtime errors that could legitimately happen (use `Result` instead).
- `assert!` is a safety net for invariants that should always hold based on design, not a substitute for proper error handling
- Use `debug_assert!` for expensive checks that verify internal logic but don't affect safety (e.g., loop invariants, complex sanity checks). Use `assert!` for invariants that must hold in production and indicate a bug if violated.

## Error Design

- Define error types as enums with granularity matching the abstraction level:
  - Application-level functions return application-level error enums
  - When bubbling up lower-level errors, wrap them explicitly (e.g., `ApiError(ApiError)`), don't flatten into strings or generic variants
- Use `thiserror` for better readability of error type definitions and automatic trait implementations
- Production Rust paths below `claw-agent` must not return `Result<_, String>` or convert errors to strings while propagating them. Keep typed enum errors and nested sources all the way up the stack. Use `Variant(#[from] LowerError)` when the lower error maps 1-to-1 into a variant; when the same lower error type can occur in multiple stages, use stage-specific variants with `#[source]` instead of flattening the context into a string.
- Only callers at or above `claw-agent` may render typed errors into strings for user-facing, FFI, logging, protocol, or other boundary output. CLI binaries, examples, and other non-production entrypoints may use `anyhow` or string errors for convenience. No other production path may use `anyhow`, `Result<_, String>`, or `error.to_string()` as an error propagation mechanism.
- **The 1-to-1 rule**: Every variant in a function's error type must be actually returnable by that function. If a variant can never occur, it should not be in that enum. Avoid "theoretical" error variants that callers must handle but will never see
- Only use `anyhow` when you need to collect errors of different types and convert them to string/display them (e.g., at program boundaries, CLI output, or logging). Do not use `anyhow` for library error types where callers need to match on specific errors
- **Propagate failures; do not absorb them silently.** A function that swallows an error — via a silent fallback, a logged-and-dropped error, or a stub that returns a fake success — hides real failures from callers. The caller cannot observe, react to, or propagate something it never receives. Return the error and let the caller decide how to handle it. This applies equally to test doubles: a stub or scripted fake that silently returns a default when it runs out of canned responses masks unexpected calls and produces false-passing tests.
- **Never add a fallback for missing configuration without an explicit user prompt.** If a required environment variable is absent, a config file cannot be read, or a required input is missing, fail immediately with a clear error message. Do not substitute a hardcoded default (e.g. `unwrap_or_else(|_| "https://api.openai.com".into())`), silently skip loading (e.g. `let _ = load_env()`), or guess at intent. Silent fallbacks mask misconfiguration, cause hard-to-diagnose misbehavior, and prevent the user from knowing their setup is wrong. Only add a fallback when the user has explicitly asked for one and the fallback value is documented at the call site.

## API Surface

- Keep configuration fields on the type that owns the behavior unless a separate config type has real semantic weight. Do not create `FooConfig` just to carry one or two fields that are only used by `FooStore`/`FooClient`; put those fields on `FooStore`/`FooClient` and expose the smallest constructor that represents the real product API.
- Do not expose public API only to make tests convenient. A test needing a smaller limit, an alternate timeout, or an artificial knob is not enough reason to add `with_*`, builder methods, or extra config structs. Prefer testing through the real default behavior, private/internal test helpers, or inputs that naturally exercise the branch. Public API entropy must come from product requirements, not test setup.

## Semantic Types and Change Scope

- Start from the responsibility of the consuming function, not from a desire to make every transport layer use the same type. Introduce a semantic type at the narrowest boundary that needs its behavior.
- A semantic wrapper must be complete: it owns every value required to perform its behavior. Do not leave required context as side parameters while claiming the wrapper represents the operation.
- Keep data and its domain-specific representation together. If a value knows how it should be rendered for a transcript, log, protocol, or UI, put that behavior on the value (commonly through a small trait implementation) and keep the consumer unaware of variant-specific formatting.
- Convert into the semantic type at the boundary where the behavior becomes relevant. Do not propagate it through schedulers, queues, persistence schemas, or unrelated APIs unless those layers themselves require its semantics.
- Prefer the smallest coherent diff. A local signature change does not authorize redesigning adjacent message types, durable state, or transport structures merely for type uniformity.
- When a requested type or trait is not found, search the repository, dependencies, and relevant history before editing. Do not invent its contract from its name; if its intended behavior still cannot be established from context, ask before creating it.

## Durable State Layout

- For persistence-enabled Rust objects, put durable fields in an `XxxState` struct and keep non-durable runtime fields as ordinary fields on `Xxx`; do not invent a `Deps` wrapper just to pass non-durable fields.
- The object owns `state: XxxState` and mutates durable data through that field. Keep a single constructor path: `Xxx::new(existing_args..., state: XxxState)`, with boot code passing checkpoint-loaded state or `XxxState::default()` when the checkpoint part is absent.
- `XxxState` is the checkpoint contract. Implement `Default` for cold boot, and let the state type choose its stable encoding. Use zero-copy/raw binary only for fixed-layout POD state; use JSON, postcard, or custom binary encoding for dynamic fields such as `String`, `Vec`, maps, `Arc`, or pointer-owning types.
- `export_state` should encode `self.state` directly instead of rebuilding ad hoc snapshots from scattered fields. Checkpoint restore should produce `XxxState`; runtime resources such as filesystem, HTTP, timers, factories, handles, and config remain normal constructor inputs.
- Missing checkpoint data may fall back to `XxxState::default()`. Corrupt, schema-mismatched, or integrity-failed checkpoint data must be surfaced or resolved by loading an older valid checkpoint, not silently defaulted.

## Clippy Lints for Panic Detection

Enable these restriction lints in `Cargo.toml` to catch panicking functions at compile time:

```toml
[lints.clippy]
unwrap_used = "deny"      # Detects .unwrap() calls
expect_used = "deny"     # Detects .expect() calls
indexing_slicing = "deny" # Detects vec[i] and slice[i] (use .get() instead)
panic = "deny"            # Detects panic!() calls
todo = "deny"             # Detects todo!() calls
unimplemented = "deny"    # Detects unimplemented!() calls
unreachable = "deny"      # Detects unreachable!() calls
arithmetic_side_effects = "deny" # Detects +, -, *, /, % that could panic
```

Note: These are **restriction lints** (off by default). They may have false positives in test code — use `#[allow(clippy::unwrap_used)]` on test modules where panicking is acceptable.

## Detecting Unsafe Code

Use the built-in Rust lint (not Clippy) to forbid `unsafe` blocks:

```toml
[lints.rust]
unsafe_code = "forbid"
```

- `forbid`: Strongest — `unsafe` code triggers a compile error and cannot be overridden with `allow`
- `deny`: `unsafe` triggers a compile error but can be overridden with `allow` in specific modules
- `warn` / `allow`: Not recommended for firmware where safety is critical

## Reducing Boilerplate Code

For converting enum members to strings (ignoring fields in the member), use `strum = { version = "0.28", features = ["derive"] }` and then use `#[derive(IntoStaticStr)]` for the enum member and you can call `(&your_enum).into()` to get static lifetime str.
