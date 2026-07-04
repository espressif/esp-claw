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
- **The 1-to-1 rule**: Every variant in a function's error type must be actually returnable by that function. If a variant can never occur, it should not be in that enum. Avoid "theoretical" error variants that callers must handle but will never see
- Only use `anyhow` when you need to collect errors of different types and convert them to string/display them (e.g., at program boundaries, CLI output, or logging). Do not use `anyhow` for library error types where callers need to match on specific errors
- **Propagate failures; do not absorb them silently.** A function that swallows an error — via a silent fallback, a logged-and-dropped error, or a stub that returns a fake success — hides real failures from callers. The caller cannot observe, react to, or propagate something it never receives. Return the error and let the caller decide how to handle it. This applies equally to test doubles: a stub or scripted fake that silently returns a default when it runs out of canned responses masks unexpected calls and produces false-passing tests.
- **Never add a fallback for missing configuration without an explicit user prompt.** If a required environment variable is absent, a config file cannot be read, or a required input is missing, fail immediately with a clear error message. Do not substitute a hardcoded default (e.g. `unwrap_or_else(|_| "https://api.openai.com".into())`), silently skip loading (e.g. `let _ = load_env()`), or guess at intent. Silent fallbacks mask misconfiguration, cause hard-to-diagnose misbehavior, and prevent the user from knowing their setup is wrong. Only add a fallback when the user has explicitly asked for one and the fallback value is documented at the call site.

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