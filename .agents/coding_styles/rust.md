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

**`Option` vs `Result` — absence is not an error:**
- Use `Option<T>` when a missing value is a **normal, expected outcome that is NOT an error** — e.g. a lookup miss, an unset/optional field, end of iteration. The caller doesn't need a reason for the absence.
- Use `Result<T, E>` when the operation can **fail**, and the failure carries a cause the caller may need to inspect, act on, or propagate.
- Anti-patterns to avoid:
  - Don't return `Option<T>` to hide a real failure — that throws away the error cause (use `Result`).
  - Don't use `Result<T, ()>` (or a single-variant error) where the "error" carries no information — that's just `Option<T>`.
- Convert at boundaries when absence *becomes* an error there: `option.ok_or(Error::Missing)` / `ok_or_else(...)`.

**`Option<T>` vs `T`'s own empty value — don't give "nothing" two encodings:**

The section above is about the *error* dimension (`Option` vs `Result`). This one is about a different, orthogonal trap: wrapping a `T` that **already has a valid empty/zero value** in an `Option`, so that "nothing" can be written two ways — `None` *and* `Some(T::empty)`. That redundant outer `Option` is a *double bottom*: one logical value with two "zeros", and you then pay to keep them in sync.

- **Prefer `T` over `Option<T>` when `T` has a natural empty/identity value that is semantically equal to "absent".** Reach for the type's own zero instead of an outer `Option`:
  - collections → the empty collection (`ToolSet::empty()`, an empty `SkillSet`, an empty `Vec`), not `Option<Set>`;
  - scalars/units → the identity element (`RetryCount(0)` meaning "tolerate none"), not `Option<u32>`;
  - behavior/dependency → a **null-object** (an allow-all `PermissionGate`, a no-op sink), not `Option<Policy>`.
- **If `T` currently *can't* represent empty, fix `T` first — add a legal empty constructor — instead of papering over it with an outer `Option`.** A type whose constructor forces a dependency it doesn't always need (e.g. `SkillSet::new(registry)` with no registry-free empty) is a *capability gap*; don't push that gap onto every caller as `Option<T>`. Give `T` an empty state (make the dependency optional / accept a null dependency), then drop the `Option`.
- **Symptoms that you've hit this anti-pattern:** you write a helper that collapses `Some(empty)` back into `None`; you sprinkle `unwrap_or(0)` / `unwrap_or_default()` / `.and_then(|t| t.render())` at every use site; you invent an error variant (e.g. `MissingTools`) for a `None` that "can't really happen"; call sites branch `Some`/`None` where an empty value would have made the behavior converge on its own.
- **Do NOT confuse this with a silent fallback.** "Never add a fallback for missing configuration" (above) forbids *guessing* an absent input's value. Using a type's own empty value for a genuinely-absent optional is not a guess — the empty *is* the value. Keep the two straight: don't substitute a made-up default for missing config, but don't wrap an already-empty-able type in `Option` either.
- **Do not copy an existing `Option<T>` shape just because nearby code uses it.** Existing code may be carrying historical looseness. Before adding or preserving an `Option`, re-check the domain invariant: is absence actually distinct from empty, or is the `Option` only compensating for a missing empty constructor/null-object? If it is only compensating, fix the owned type or boundary first.
- **Exceptions (keep the `Option`):**
  - `T` has **no** empty value — a newtype id or handle (`Option<AgentId>`, root has no parent), an event/payload with no neutral element (`Option<ApprovalNeeded>`). The strongest form is a type that *constructs away* the zero: `Option<NonZeroU32>` (e.g. a `ToolRetryCount`) is **correct**, not a double bottom — `NonZeroU32` cannot be `0`, so `None` is the *only* encoding of "none" and the niche-optimized `Option` is the same size as the bare integer. Contrast `RetryCount(u32)`, where `0` is a genuine present count ("tolerate zero"), not absence — there a bare `u32` is right. The deciding question is whether `0`/empty *is a real value* or *means absent*.
  - `None` and `T::empty` are **semantically distinct** — optional free-text where "unset" differs from `""` (a supervision `name`, a rejection `note`), or where the empty value is itself an invalid input.
  - The `Option` is a **transient accumulator** ("produced anything this tick yet?"), not a stored empty value.
  - The `Option` is really encoding **FSM state** — fold it into the state enum instead (`AwaitingApproval(ApprovalId)`), which is the *make-illegal-states-unrepresentable* fix, not this one.

## Agent Layering Boundaries (`claw-core`)

- `BaseAgent` is the low-level command/tick/tool/permission/context-adapter host. It should not own a concrete conversation-memory strategy, summary cursor, rolling-summary policy, or built-in history adapter pair.
- `GenericAgent` owns the default conversation-memory strategy. It constructs and wires the recent-history adapter, rolling-summary adapter, and their shared cursor so the summarized prefix and verbatim tail stay coordinated without leaking that coordination state into `BaseAgentConfig`.
- Do not expose internal agent state just to help a CLI, test, or adapter inspect the implementation. Prefer driving through `AgentCommand` and observing `TickOutcome`; keep helper surfaces `pub(crate)` unless they are an intentional external boundary.
- Host CLIs should exercise the production-level agent assembly path (`GenericAgent` or orchestrator) instead of teaching `BaseAgent` high-level policies to make manual testing convenient.

## Naming

- Follow standard Rust naming conventions (snake_case for functions/variables, CamelCase for types/traits, SCREAMING_SNAKE_CASE for constants, etc.) as the first priority
- Once Rust ergonomics are satisfied, prefer full descriptive names over abbreviations (e.g., `configuration` over `cfg`, `response` over `resp`, `error` over `err`)

**File and Directory Names:**
- **Crate names** (in `Cargo.toml`): use **kebab-case with hyphens** (e.g., `claw-api`, `serde-json`). Required by crates.io and ecosystem standards
- **Internal files and directories**: use **snake_case with underscores** (e.g., `my_module.rs`, `claw_core/`). This matches the module names in code (`mod claw_core;`) and follows Rust API guidelines

## API Ergonomics — Accept Traits, Not Concrete Types

**General principle:** design function signatures around the most general *trait* that captures what the function actually needs, rather than a specific concrete type. Be liberal in what you accept (broad trait bound) and specific in what you return. This is the Rust API Guidelines' "be generic" rule (C-GENERIC): the parameter should describe a *capability*, not a *type*.

This is one idiom with many concrete forms. Reach for the trait bound that matches the capability you need:

- Read-only string → `impl AsRef<str>` instead of `&str`
- Filesystem path → `impl AsRef<Path>` instead of `&Path` (accepts `&str`, `String`, `PathBuf`, `&Path`, …)
- Value to store/own → `impl Into<String>` instead of `String`
- Sequence to consume → `impl IntoIterator<Item = T>` instead of `&[T]` / `Vec<T>`
- Returned/streamed sequence → `impl Iterator<Item = T>`
- Formatting/printing → `impl Display` / `impl Debug`
- Callback → `impl Fn(...) -> ...` (or `FnMut` / `FnOnce`) instead of a fn pointer
- Fallible conversion → `impl TryInto<T>`

The payoff: callers pass whatever they already hold with no `.as_ref()` / `.to_string()` / `.collect()` boilerplate, and the signature documents the real requirement. Inside the function, normalize once (e.g. `let path = path.as_ref();`).

**Don't over-apply.** Use a plain concrete type when: it's a hot path or trait-internal method (avoid monomorphization bloat / keep it object-safe), a single concrete type is always passed, or the generic bound makes the signature harder to read than it helps.

## Functional Style & Readability

**Combinators over verbose `match`:**
- Prefer combinators for simple `Option`/`Result` handling instead of a full `match`: `unwrap_or`, `unwrap_or_else`, `unwrap_or_default`, `map`, `map_or`, `and_then`, `or_else`, `filter`, `ok_or`, and the `?` operator. They state intent in one line and avoid the indentation a `match` block adds.
- For boolean pattern checks, use `matches!(value, Pattern)` instead of `match value { Pattern => true, _ => false }`.

**Lazy vs eager defaults (correctness, not just style):**
- Use the `_else` variants when the fallback is expensive or has side effects, so it's computed only when needed: `unwrap_or_else(|| …)`, `ok_or_else(|| …)`, `map_or_else(…)`. `unwrap_or(expensive())` **always** evaluates `expensive()` — use it only for cheap constants.

**Iterator pipelines over manual loops:**
- Prefer iterator chains over index loops where it reads clearly. Iterators are zero-cost, fuse/optimize well, and sidestep manual indexing (which can panic — see `indexing_slicing`).
- Reach for the *named* adapter instead of a hand-rolled loop: `filter_map`, `find_map`, `flat_map`, `enumerate`, `zip`, `take_while`/`skip_while`, `any`/`all`, `position`, `fold`/`try_fold`, `partition`.
- Short-circuit fallible pipelines by collecting into a `Result`/`Option`: `iter.map(try_thing).collect::<Result<Vec<_>, _>>()?`. Use `Option::transpose` / `Result::transpose` to swap `Option<Result>` ↔ `Result<Option>`.
- Return `impl Iterator<Item = …>` to expose a lazy pipeline without forcing an intermediate `collect`.

**Flatten control flow (reduce nesting):**
- Use `?` for error/None propagation, and `let … else { return … }` / `if let` / `while let` to handle the absence case up front (guard clauses) instead of wrapping the happy path in deep `if`/`match` nesting.
- Rust is expression-oriented: let `if`, `match`, `loop`, and blocks *evaluate to* a value (`let x = if cond { a } else { b };`) instead of declaring `let mut x;` and assigning in each branch.

**Formatting:**
- Format method chains one step per line with a leading dot for readable pipelines.
- Use struct update syntax (`Foo { field, ..Default::default() }`) for readable construction.

**Don't force it.** When branch logic is complex, has side effects, or a long combinator chain becomes a "callback pyramid," a `match` or an early return is clearer. Optimize for the reader — the win here is **readability**; for `Option`/`Result` combinators the codegen is typically identical to a `match`.

## Newtypes for Domain Ids & Units

- **Wrap domain identifiers and units in newtypes** instead of passing bare primitives — e.g. `struct IterationId(usize)` rather than a raw `usize`. This prevents mixing up values that share an underlying type (a `TaskId` can't be passed where a `StepId` is expected), documents intent at call sites, and gives a home for parsing/validation/formatting methods.
- The repo already does this via the `define_prefixed_id!` macro: `AgentId`, `ApprovalId`, `IterationId`, `TaskId`, `StepId`, `WorkerId`, `SessionId`. Each wraps a **`u32`** (a logical id, not a memory index — see *Integer Type Selection*), never `usize`. Prefer reusing that pattern for new ids. Apply the same idea to units (durations, byte sizes, counts) where confusion is plausible.
- **Every monotonic domain-id counter uses `define_id_allocator!` — no hand-rolled counters.** A counter that hands out ids should hold the id newtype itself, not a bare integer, so it follows the id's representation and can't drift into a raw-`usize` assumption. `define_id_allocator!(FooIdAllocator(FooId), FooId(1))` generates the lock-free "store the newtype, post-increment (`next(&mut self)`), read position (`peek`), resume (`starting_at`)" logic in one place. Reach for the macro even for a single-site counter: uniformity (one grep-able pattern, no re-derived `saturating_add`) outweighs the "it's only one line" argument. The current allocators — `AgentIdAllocator`, `SessionIdAllocator`, `IterationIdAllocator`, `ApprovalIdAllocator`, `TurnIdAllocator` — are all macro-generated.
- **A generator (macro/helper) must not bake in a synchronization policy; choose the lock at the caller's layer.** `define_id_allocator!` deliberately produces a lock-free, non-`Clone`/`Copy` counter — an allocator's job is to never repeat, and a copied counter forks silently. The *counter type* is always the macro; *where the lock goes* is the caller's decision, driven by the sharing model (the same rule as *"has an outer lock? reuse it"* in the `Box` vs `Arc` guidance):
  - **Single `&mut self` owner** → hold the macro allocator as a plain field; `&mut` is the exclusivity, no lock (e.g. `BaseAgent`'s `approvals: ApprovalIdAllocator`, `iterations: IterationIdAllocator`).
  - **One field of an already-locked state** → put the macro allocator *inside that same lock* so allocation and the state mutation are one critical section. Don't add a second, independent lock for state that logically belongs together (see `SessionStore`: the allocator lives in the same `Mutex` as the session map; `TranscriptStore`: in the same lock as the turn log).
  - **Genuinely shared across independent owners with no common enclosing lock** (cloned into several holders) → wrap the macro allocator in *the caller's own* `Arc<Mutex<_>>` at that one shared owner (see `AgentIdAllocator`, cloned into every per-session instance and drawn while the orchestrator's map lock is released).
- **Persisting an allocator: keep it out of the serialized struct.** The macro type is intentionally not `Serialize`. To persist position, write `allocator.peek()` into a plain id-newtype field on your on-disk record and rebuild with `starting_at` on load — the wire format stays a bare id, and the allocator abstraction never leaks into serde (see `TranscriptStore`'s manifest `next_id`).
- **A second lock over the same logical state is "lock pollution."** Two independent locks guarding what is really one state (a map + its id counter) means the two mutations aren't atomic together, add a lock-ordering surface, and usually carry a gratuitous `Arc`. Fold them under one lock unless there is a real reason they must move independently.
- These newtypes are typically small POD — derive `Copy` (see derive discipline) so they stay ergonomic.
- **Thread the newtype end-to-end; unwrap only at the one true boundary.** Once a value is a newtype (`RetryCount`, `AgentId`), carry *that type* through every intermediate struct, config, and function that only passes it along. Convert to the raw primitive (`.get()`, `.0`, `try_into()`) at the single call site that genuinely needs it — typically a lower crate's API that takes `u32`/`&str`. A newtype that decays back to a bare `u32`/`String` in a mid-chain `Config` field is a newtype you didn't finish building: the type safety is lost exactly where the value is most likely to be confused. Inconsistency across sibling modules (one holds `RetryCount`, the next holds `u32` for the same value) is the tell.

## Clone / Copy & Derive Discipline

- **Derive `Debug` on public types** so callers can log and assert on them. Add `Clone`, `PartialEq`/`Eq`, `Hash` where they're cheap and meaningful; derive `Serialize`/`Deserialize` only at the (de)serialization boundary.
- **Derive `Copy` only for small "plain old data"** — a handful of scalar/`Copy` fields with value semantics (ids, enums, small config). Don't derive `Copy` on types holding `String`/`Vec`/`Arc` or on anything large; implicit bitwise copies of big types hurt readability and can pessimize.
- **No gratuitous `.clone()`.** Prefer borrowing (`&T`) over cloning; clone only when you genuinely need an independent owned value. A `.clone()` in a hot path or loop is a smell — pass a reference, restructure ownership, or use `Rc`/`Arc` for shared read-only data.
- Take parameters by the weakest sufficient form: `&T` to read, `&mut T` to mutate in place, `T` only when the function must own/consume it. Don't take `T` then immediately clone inside.
- **`Box<dyn T>` vs `Arc<dyn T>` for an owned trait object — ask "is there a second owner *today*?"** Type erasure alone does **not** require `Arc`. Use `Box<dyn T>` when the value has exactly one owner and callers only need `&self`/`&mut self` (the common case for a field that erases a generic like `F: ClawFs`). Reach for `Arc<dyn T>` only when the value genuinely has multiple live owners *now* — shared read-only state fanned out to N holders (e.g. one `inherited_context: Arc<[Block]>` referenced by every agent in a session), or a handle cloned into a spawned task. Do **not** pick `Arc` for a *hypothetical* future second owner ("might share later") — that is speculative; switch the type when the second owner actually arrives.
- **Watch for a double `Arc`.** Wrapping a handle that is *already* `Arc`-backed internally (a store defined as `struct Store { inner: Arc<Inner> }` whose `Clone` bumps that inner `Arc`) in an outer `Arc<dyn T>` adds a second refcount and pointer hop for zero extra sharing — the inner `Arc` already provides it. If the real sharing lives inside the value, the outer wrapper should be a `Box`.

## Numeric Casts & Conversions

- **Avoid the `as` operator for numeric conversions.** `as` silently truncates or wraps with no error: `300u32 as u8 == 44`, `-1i32 as u32 == 4294967295`, `1.9f32 as i32 == 1`. These are exactly the bugs that bite on embedded width mismatches.
- Prefer trait conversions:
  - **Lossless / widening** (`u8 → u32`, `u32 → u64`): use `From`/`Into` (`u32::from(x)` / `x.into()`).
  - **Narrowing / possibly-lossy** (`u64 → u32`, `usize → u32`): use `TryFrom`/`try_into()` and handle the `Err` (the classic `usize` ↔ fixed-width situation at a memory-size / FFI boundary).
- Reserve `as` only for cases where truncation is *intended and documented*, or where no trait alternative exists (e.g. some pointer or enum-discriminant casts). When you do use it, add a comment explaining why the cast is safe.

## Integer Type Selection (`usize` vs fixed-width)

- **Use `usize`/`isize` only for memory-related quantities** — anything tied to a memory location or in-memory size: indices, slice/collection lengths, byte offsets, buffer capacities, pointer arithmetic. These are pointer-width by definition, so the type follows the platform's address size.
- **Do not use `usize` for logical values** (ids, counts, versions, flags, wire/serialized fields, durations expressed as raw integers, etc.). `usize` is 32-bit on the device target and 64-bit on the host, so reusing it for logical data makes ranges and serialized layouts platform-dependent and invites silent truncation across the FFI/host boundary.
- **For logical values, pick a fixed-width type from the actual capacity and use case** — `u8`/`u16`/`u32`/`u64` (or the signed variants) sized to the value's real range, not "whatever's convenient." Smaller types document the expected range and save space in structs and on the wire; reach for `u64` when the count can realistically grow large.
- Newtypes (see *Newtypes for Domain Ids & Units*) should wrap the fixed-width type chosen this way. Where a logical id and a memory index must interoperate, convert explicitly with `TryFrom`/`try_into()` rather than papering over the difference with `usize`.

## Static vs Dynamic Dispatch

- **Prefer static dispatch** — generics (`fn f<T: Trait>(...)`) and `impl Trait` — over `dyn Trait`. Static dispatch is monomorphized: calls are direct (inlinable, no vtable indirection), which is the default you want for performance and for `no_std`/embedded.
- **Use `dyn Trait` deliberately, when static dispatch doesn't fit:**
  - heterogeneous collections (`Vec<Box<dyn Trait>>`),
  - to **bound monomorphization/code-size bloat** when a generic is instantiated for many types (a real concern on flash-constrained targets),
  - to break deep generic propagation or keep a type object-safe at an API boundary,
  - for pluggable drivers/dependency injection where the concrete type is chosen at runtime.
- Trade-off in one line: **generics = faster, larger binary; `dyn` = smaller binary, vtable indirection.** On embedded, weigh both — default to generics, switch to `dyn` where code size or flexibility wins.
- Keep traits **object-safe** if they're meant to be used as `dyn` (no generic methods, no `Self`-by-value returns, etc.).

## API Boundary & Public Surface

- **Minimize the public surface.** Expose only what outside callers actually need. Default to private; promote to `pub(crate)` for cross-module-internal use, and `pub` only for the intended boundary. A smaller surface is easier to keep stable and to evolve without breaking callers.
- **Easy by default, extensible when needed.** The common path should require minimal setup (sensible defaults, few required arguments), while still allowing advanced callers and tests to swap low-level dependencies — "drivers" such as transports, backends, clocks, RNGs — via injection. (Driver here is just an example for the low-level necessary details a caller might override.)
- **Defaults via `Default`, readability via `with_xxx()`.** Implement the `Default` trait for zero-config construction. Because `Default::default()` is opaque about *what* it sets, complement it with named builder-style methods (`with_timeout(...)`, `with_retry(...)`, …) that apply readable, intention-revealing defaults and overrides on top of `Default`.
- **Finalize the boundary when an object/crate is "done."** Re-audit every `pub`: shrink the surface to the minimum necessary before considering the module/crate complete.
- **Delete dead threaded parameters.** If a value is passed through constructors and carried across structs but *no consumer ever reads it* (a field kept "for future use," a request field no policy inspects, an id threaded to a layer that only forwards it), remove it along its whole chain — struct field, constructor arg, and every call site. Re-add it *with the right type* when a real consumer appears. Threading a value nothing reads is speculative generality: it inflates signatures, invites the wrong type (a bare `u64`/`String` nobody validates, precisely because nobody uses it), and lies to the reader about what matters. The test: grep for a *read* of the field (`x.field` used in a condition/computation), not just its construction — construction-only means dead.
- **Keep a concept in its own layer; pass an opaque payload across a lower boundary and translate at the edge.** A lower/transport layer (a DTO, a capability channel message) must not carry a *higher* layer's domain enum — that couples the two and drags the enum's crate downward. Have the lower layer carry an opaque, broad payload (e.g. `extra_context: Option<String>`) and let the owning layer translate it into its domain type (`DeliveryKind::from_extra_context(...)`) at the boundary, defaulting explicitly (with a trace) for unrecognized input. The domain enum then lives only in the crate that owns the concept.
- **Document the boundary with rustdoc — public items only.** Write doc comments and runnable `/// # Examples` for **public** APIs (the boundary callers consume). Do not spend rustdoc/examples on private internals; comment those only where intent is non-obvious (see the comment guidance in `making_code_changes`).
- **Examine the public API shape** with `cargo public-api` (install if missing: `cargo +stable install cargo-public-api`):

  ```bash
  cargo public-api \
    -p <path-to-crate> \
    --target x86_64-unknown-linux-gnu \
    --omit blanket-impls,auto-trait-impls,auto-derived-impls
  ```

## Enforcing Call Order: Typestate vs RAII

- **Default to RAII.** In most cases, ownership + `Drop` already enforces correct usage: acquire a resource by constructing a value, use it through its methods, release it automatically when it drops. Reach for RAII first — it's the simplest tool.
- **Use the typestate pattern (a type-level finite state machine) only when an API requires a strict *call order* that RAII alone can't express** — e.g. `open_xxx()` returns a value in a state where only a specific set of methods is callable, and calling them transitions the type to the next state. Encode each state as a distinct type (often via generic markers) so illegal sequences fail to compile. Example in this repo: `OrchestratorBuilder<Channels, Llm>` only exposes `build()` once both `Channels` and `Llm` are in their "set" states.
- **It's a compile-time-check tool, not a default.** Typestate buys *static* prevention of misuse at the cost of more types and signatures. Use it where wrong ordering is a real, likely bug; don't impose it where RAII or a plain runtime check is clearer.

## Crate Design: Inbound vs Outbound Boundaries

Rust is integrated as an ESP-IDF *component*, so the Rust workspace (`framework/runtime/`) is organized around two FFI boundaries with a pure-Rust core in between.

- **Inbound crates (C / OS → Rust).** Convert C and operating-system facilities into ergonomic Rust APIs and dependency-injection traits, including OS-level abstraction, with per-target implementations (ESP-IDF and Linux/host). The core depends on the *traits*, never on a platform directly.
  - `claw-interface` — shared types + DI traits (`esp_err`, `ClawEvent`, `EventPublisher`, `ClawHttp`).
  - `claw-sys` — thin IDF shims std can't express (the `ESP_LOGx` log sink, i.e. the C↔Rust logging bridge, and the `esp_http_client` `ClawHttp` driver). Linux/host impls plug into the same traits for tests.
- **Outbound crate (Rust → C).** Converts the Rust APIs back into a C ABI for the firmware's C callers, with **explicit init/deinit lifetimes and reference ownership**. Prefer **opaque handle types** (an opaque pointer) over structs that expose internal fields/layout.
  - `claw_capi` — the single C ABI layer between the pure-Rust claw modules and the C callers.
- **Pure-Rust core in between.** Depends only on inbound traits, contains no platform/FFI details, and stays unit-testable on the host: e.g. `claw-api`, `claw_core`, `claw_cap`, `claw-memory`, `claw-log` (the `log` facade backend + flat-tree `tracing` subscriber that drive `claw-sys`'s `ESP_LOGx` sink, plus the compile-time `log_max_*` / `trace_max_*` level ceilings).

Rules:
- Keep platform/FFI specifics **at the boundaries**. Core crates must not call C or platform APIs directly — depend on an inbound trait and inject the implementation.
- Outbound: every exported handle has a clear create/destroy pair; ownership and reference validity across the ABI are explicit. **Do not leak Rust struct internals across the C ABI** — hand out opaque handles.
- Provide both an ESP-IDF and a Linux/host implementation of each inbound trait so core logic can be exercised off-device.

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