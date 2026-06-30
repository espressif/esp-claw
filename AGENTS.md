# Agents.md

This file provides guidance to agents when working with code in this repository.

## Project Overview

ESP-Claw is an ESP-IDF firmware project for running an AI agent framework on Espressif IoT devices. The main application is `application/edge_agent/`; reusable firmware components live under `components/`. The repo also contains board definitions, build-time FATFS content, documentation, and the embedded device settings UI.

## Development Commands

Export ESP-IDF before firmware work:

```bash
. $IDF_PATH/export.sh
```

Generate board manager files and build from the app directory:

```bash
cd application/edge_agent
idf.py bmgr -c ./boards -b esp32_S3_DevKitC_1
idf.py build
idf.py flash monitor
```

Docs site:

```bash
cd docs
pnpm install
pnpm build
pnpm dev
```

Embedded settings UI:

```bash
cd application/edge_agent/components/http_server/frontend_source
pnpm build
pnpm typecheck
```

## High-Level Architecture

### Boot and Runtime Flow

The main entry point is `application/edge_agent/main/main.c`. 

### Core Data Flow

1. IM channels, scheduler jobs, Lua scripts, startup hooks, or CLI commands publish events or submit requests.
2. `claw_event_router` matches events against the DATA root's `router_rules/router_rules.json` and can call capabilities, run scripts, run the agent, send messages, emit events, or drop events.
3. `claw_core` builds context from memory, session history, skills, and other providers; calls the configured LLM backend; executes capability tool calls; persists context; and returns responses.
4. Outbound messages are routed back through registered IM bindings or local/web channels.

### Iteration semantics

- Use `IterationId` only. Do not introduce legacy `TurnId` aliases or `turn_id` fields.
- An iteration is never resumed after preemption.
- A preempted iteration is terminal.
- The next action must happen in a new iteration.
- `IterationLoop` (Layer 3) only detects preemption, ends the iteration, and returns `patch` + `reason`. Layer 1/2 merge patches, rebuild context, and start the next iteration.
- Preemption checkpoints in `iteration_loop`: `BeforeLlmHttp`, `InLlmHttpAbort`, `AfterLlmBeforeTool`, `BeforeTool`. While a tool is running (`RunningTool`), do not preempt; let the tool finish and handle new input on the next iteration unless the tool supports cooperative cancellation.

## Key Subsystems

- **Application shell** (`application/edge_agent/main/main.c`, `components/common/app_claw/`): boot flow, storage paths, capability registration, Lua module registration, CLI, and agent startup.
- **Agent core** (`components/claw_modules/claw_core/`): request queue, context building, LLM backend runtime, tool-call loop, media inference, interrupts, context persistence, and response delivery.
- **Event router** (`components/claw_modules/claw_event_router/`): declarative event routing and actions backed by router rules in FATFS.
- **Capability registry** (`components/claw_modules/claw_cap/`): common registration and dispatch layer for model-callable capabilities.
- **Capabilities** (`framework/capabilities/`): concrete agent capabilities such as Lua execution, files, IM platforms, MCP, skill management, router management, scheduler, session management, time, HTTP requests, web search, system, and LLM inspection.
- **Memory** (`components/claw_modules/claw_memory/`): session history, profile/long-term memory providers, memory persistence, request gating, and stage notes.
- **Skills** (`components/claw_modules/claw_skill/`, component `skills/` directories): user-facing skill documents and activation state.
- **Lua modules** (`components/lua_modules/`): Lua drivers and higher-level modules for hardware, media, HTTP server, storage, threading, JSON, board manager, and capability calls.
- **Board manager** (`application/edge_agent/boards/`): board metadata, peripheral YAML, board setup code, board defaults, optional local components, and optional board FATFS overlays.
- **FATFS images** (`application/edge_agent/fatfs_image/`): build-time source trees for the read-only SYSTEM image and writable DATA seed image.
- **HTTP config service** (`application/edge_agent/components/http_server/`): local device configuration server and embedded frontend.

### Runtime Path Rules

The firmware uses two logical filesystem roots, configured at boot through `claw_paths`:

- `CLAW_PATH_SYSTEM` is mounted at `/system`. It is read-only and contains firmware-baked skills, skill assets, built-in Lua modules, Lua docs/tests, board image overlays, and `.recovery` seed files.
- `CLAW_PATH_DATA` is the writable storage root. It is `/fatfs` when flash storage is used, or the board-manager SD card mount point when an SD card is available.
- Never hard-code `/fatfs` for writable paths in reusable code or docs. Use `claw_paths_join(CLAW_PATH_DATA, ...)` in C and `storage.get_root_dir()` plus `storage.join_path(...)` in Lua.
- Firmware-baked skill scripts must be referenced with `{CUR_SKILL_DIR}/scripts/...` inside `SKILL.md`; do not write fixed `/fatfs/skills/...` paths.
- Runtime-installed/user skills live under the DATA root's `skills/`. Firmware-baked skills live under `/system/skills/`; the skill registry scans both, with DATA skills taking priority when ids conflict.
- Router rules, scheduler rules, memory, sessions, inbox, and user-generated files live under DATA. Recovery defaults are stored under `/system/.recovery` and copied into DATA only when missing.
- Built-in Lua libraries are staged under `/system/scripts/builtin/lib`; generated Lua module docs/tests are bundled into the `builtin_lua_modules` skill and should be accessed via that skill's `{CUR_SKILL_DIR}` paths.
- Board-specific `boards/<vendor>/<board>/fatfs_image/` content overlays the SYSTEM image at build time. Board image content does not target DATA and hidden board folders are not considered.

## Project-Specific Notes

- Architecture constraints: [`design.md`](.agents/design.md)
- docs guide: [`docs.md`](.agents/docs.md)
- Common gotchas: [`gotchas.md`](.agents/gotchas.md)
- Specs (`.agents/spec/`):
  - lua module spec: [lua-module-spec.md](.agents/spec/lua-module-spec.md)
  - claw skill spec: [claw-skill-spec.md](.agents/spec/claw-skill-spec.md)

## General Engineering Rules

- Use modular design. Each module should have clear responsibilities, ownership, and boundaries.
- Keep source files under 1500 lines where practical; split files by responsibility when they grow beyond that. Exception: Rust files may exceed 1500 lines, especially when in-crate `#[cfg(test)]` test modules (stripped from non-test builds) live alongside the code they test, per Rust convention.
- Keep functions focused and reviewable; split large functions instead of adding deeply nested branches.
- Avoid magic numbers and magic strings. Use named constants, enums, macros, Kconfig options, or shared config keys.
- Prefer explicit ownership and explicit data flow over hidden global state.
- Keep public headers small and avoid exposing private implementation details.
- Avoid circular dependencies between components and modules.
- Check return values, handle allocation failures, and clean up partially initialized resources.
- Protect shared mutable state with documented ownership or synchronization.

## Code Style

### C (ESP-IDF)

- Implement the module in ESP-IDF using C-style object-oriented design, not C++.
- Represent each module as an object with an opaque handle: typedef struct xxx_t *xxx_handle_t.
- The header should expose only the handle, config, events, callbacks, and public APIs.
- Define struct xxx_t only in the .c file to store object state and resources.
- Use ESP-IDF-style APIs: xxx_create/delete/start/stop/read/write/set/get.
- Use xxx_handle_t handle as the first parameter of object methods.
- Prefer esp_err_t as the return type for public APIs.
- Use const xxx_config_t *config as create input and xxx_handle_t *ret_handle as output.
- Resources must be allocated in create and fully released in delete.
- Internal resources may include memory, GPIO, I2C, SPI, timers, tasks, queues, and mutexes.
- Protect shared state with mutexes or semaphores when accessed by multiple tasks.
- Register callbacks with xxx_register_cb(), using handle, event, and user_ctx.
- For polymorphism, use an xxx_ops_t function pointer table and put base struct as the first member.

### Rust

Rust Best Practices by Rustacean Finn(Ziheng) Sheng.

- Prefer returning errors via `Result` over panicking. Only panic for truly unreachable/buggy states.
- Use `unimplemented!()` for code paths that are intentionally not implemented and will never be (e.g., unsupported enum variants that should never occur).
- Use `todo!()` for stubs that are meant to be implemented in the future (clear marker for "work in progress").
- Do NOT use `unimplemented!()` or `todo!()` in places where a proper error variant can be returned via `Result`. Panics are for bugs, not expected error conditions.

#### Assertions (`assert!`, `assert_eq!`, etc.)

- Use `assert!` only for conditions that **logically cannot occur**, but **programmatically you cannot guarantee**. For example: "Memory usage will never exceed 512 bytes in this design, but the addition operation could theoretically overflow" — use `assert!` to document and verify this assumption.
- Do NOT use `assert!` for expected runtime errors that could legitimately happen (use `Result` instead).
- `assert!` is a safety net for invariants that should always hold based on design, not a substitute for proper error handling
- Use `debug_assert!` for expensive checks that verify internal logic but don't affect safety (e.g., loop invariants, complex sanity checks). Use `assert!` for invariants that must hold in production and indicate a bug if violated.

#### Error Design

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

#### Naming

- Follow standard Rust naming conventions (snake_case for functions/variables, CamelCase for types/traits, SCREAMING_SNAKE_CASE for constants, etc.) as the first priority
- Once Rust ergonomics are satisfied, prefer full descriptive names over abbreviations (e.g., `configuration` over `cfg`, `response` over `resp`, `error` over `err`)

**File and Directory Names:**
- **Crate names** (in `Cargo.toml`): use **kebab-case with hyphens** (e.g., `claw-api`, `serde-json`). Required by crates.io and ecosystem standards
- **Internal files and directories**: use **snake_case with underscores** (e.g., `my_module.rs`, `claw_core/`). This matches the module names in code (`mod claw_core;`) and follows Rust API guidelines

#### API Ergonomics — Accept Traits, Not Concrete Types

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

#### Functional Style & Readability

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

#### Newtypes for Domain Ids & Units

- **Wrap domain identifiers and units in newtypes** instead of passing bare primitives — e.g. `struct IterationId(usize)` rather than a raw `usize`. This prevents mixing up values that share an underlying type (a `TaskId` can't be passed where a `StepId` is expected), documents intent at call sites, and gives a home for parsing/validation/formatting methods.
- The repo already does this via the `define_prefixed_id!` macro: `IterationId`, `TaskId`, `StepId`, `WorkerId`, `SessionId`. Prefer reusing that pattern for new ids; apply the same idea to units (durations, byte sizes, counts) where confusion is plausible.
- These newtypes are typically small POD — derive `Copy` (see derive discipline) so they stay ergonomic.

#### Clone / Copy & Derive Discipline

- **Derive `Debug` on public types** so callers can log and assert on them. Add `Clone`, `PartialEq`/`Eq`, `Hash` where they're cheap and meaningful; derive `Serialize`/`Deserialize` only at the (de)serialization boundary.
- **Derive `Copy` only for small "plain old data"** — a handful of scalar/`Copy` fields with value semantics (ids, enums, small config). Don't derive `Copy` on types holding `String`/`Vec`/`Arc` or on anything large; implicit bitwise copies of big types hurt readability and can pessimize.
- **No gratuitous `.clone()`.** Prefer borrowing (`&T`) over cloning; clone only when you genuinely need an independent owned value. A `.clone()` in a hot path or loop is a smell — pass a reference, restructure ownership, or use `Rc`/`Arc` for shared read-only data.
- Take parameters by the weakest sufficient form: `&T` to read, `&mut T` to mutate in place, `T` only when the function must own/consume it. Don't take `T` then immediately clone inside.

#### Numeric Casts & Conversions

- **Avoid the `as` operator for numeric conversions.** `as` silently truncates or wraps with no error: `300u32 as u8 == 44`, `-1i32 as u32 == 4294967295`, `1.9f32 as i32 == 1`. These are exactly the bugs that bite on embedded width mismatches.
- Prefer trait conversions:
  - **Lossless / widening** (`u8 → u32`, `u32 → u64`): use `From`/`Into` (`u32::from(x)` / `x.into()`).
  - **Narrowing / possibly-lossy** (`u64 → u32`, `usize → u32`): use `TryFrom`/`try_into()` and handle the `Err` (this is the `usize` ↔ `u32` situation — e.g. `IterationId`).
- Reserve `as` only for cases where truncation is *intended and documented*, or where no trait alternative exists (e.g. some pointer or enum-discriminant casts). When you do use it, add a comment explaining why the cast is safe.

#### Integer Type Selection (`usize` vs fixed-width)

- **Use `usize`/`isize` only for memory-related quantities** — anything tied to a memory location or in-memory size: indices, slice/collection lengths, byte offsets, buffer capacities, pointer arithmetic. These are pointer-width by definition, so the type follows the platform's address size.
- **Do not use `usize` for logical values** (ids, counts, versions, flags, wire/serialized fields, durations expressed as raw integers, etc.). `usize` is 32-bit on the device target and 64-bit on the host, so reusing it for logical data makes ranges and serialized layouts platform-dependent and invites silent truncation across the FFI/host boundary.
- **For logical values, pick a fixed-width type from the actual capacity and use case** — `u8`/`u16`/`u32`/`u64` (or the signed variants) sized to the value's real range, not "whatever's convenient." Smaller types document the expected range and save space in structs and on the wire; reach for `u64` when the count can realistically grow large.
- Newtypes (see *Newtypes for Domain Ids & Units*) should wrap the fixed-width type chosen this way. Where a logical id and a memory index must interoperate, convert explicitly with `TryFrom`/`try_into()` rather than papering over the difference with `usize`.

#### Static vs Dynamic Dispatch

- **Prefer static dispatch** — generics (`fn f<T: Trait>(...)`) and `impl Trait` — over `dyn Trait`. Static dispatch is monomorphized: calls are direct (inlinable, no vtable indirection), which is the default you want for performance and for `no_std`/embedded.
- **Use `dyn Trait` deliberately, when static dispatch doesn't fit:**
  - heterogeneous collections (`Vec<Box<dyn Trait>>`),
  - to **bound monomorphization/code-size bloat** when a generic is instantiated for many types (a real concern on flash-constrained targets),
  - to break deep generic propagation or keep a type object-safe at an API boundary,
  - for pluggable drivers/dependency injection where the concrete type is chosen at runtime.
- Trade-off in one line: **generics = faster, larger binary; `dyn` = smaller binary, vtable indirection.** On embedded, weigh both — default to generics, switch to `dyn` where code size or flexibility wins.
- Keep traits **object-safe** if they're meant to be used as `dyn` (no generic methods, no `Self`-by-value returns, etc.).

#### API Boundary & Public Surface

- **Minimize the public surface.** Expose only what outside callers actually need. Default to private; promote to `pub(crate)` for cross-module-internal use, and `pub` only for the intended boundary. A smaller surface is easier to keep stable and to evolve without breaking callers.
- **Easy by default, extensible when needed.** The common path should require minimal setup (sensible defaults, few required arguments), while still allowing advanced callers and tests to swap low-level dependencies — "drivers" such as transports, backends, clocks, RNGs — via injection. (Driver here is just an example for the low-level necessary details a caller might override.)
- **Defaults via `Default`, readability via `with_xxx()`.** Implement the `Default` trait for zero-config construction. Because `Default::default()` is opaque about *what* it sets, complement it with named builder-style methods (`with_timeout(...)`, `with_retry(...)`, …) that apply readable, intention-revealing defaults and overrides on top of `Default`.
- **Finalize the boundary when an object/crate is "done."** Re-audit every `pub`: shrink the surface to the minimum necessary before considering the module/crate complete.
- **Document the boundary with rustdoc — public items only.** Write doc comments and runnable `/// # Examples` for **public** APIs (the boundary callers consume). Do not spend rustdoc/examples on private internals; comment those only where intent is non-obvious (see the comment guidance in `making_code_changes`).
- **Examine the public API shape** with `cargo public-api` (install if missing: `cargo +stable install cargo-public-api`):

  ```bash
  cargo public-api \
    -p <path-to-crate> \
    --target x86_64-unknown-linux-gnu \
    --omit blanket-impls,auto-trait-impls,auto-derived-impls
  ```

#### Enforcing Call Order: Typestate vs RAII

- **Default to RAII.** In most cases, ownership + `Drop` already enforces correct usage: acquire a resource by constructing a value, use it through its methods, release it automatically when it drops. Reach for RAII first — it's the simplest tool.
- **Use the typestate pattern (a type-level finite state machine) only when an API requires a strict *call order* that RAII alone can't express** — e.g. `open_xxx()` returns a value in a state where only a specific set of methods is callable, and calling them transitions the type to the next state. Encode each state as a distinct type (often via generic markers) so illegal sequences fail to compile. Example in this repo: `OrchestratorBuilder<Channels, Llm>` only exposes `build()` once both `Channels` and `Llm` are in their "set" states.
- **It's a compile-time-check tool, not a default.** Typestate buys *static* prevention of misuse at the cost of more types and signatures. Use it where wrong ordering is a real, likely bug; don't impose it where RAII or a plain runtime check is clearer.

#### Crate Design: Inbound vs Outbound Boundaries

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

#### Clippy Lints for Panic Detection

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

#### Detecting Unsafe Code

Use the built-in Rust lint (not Clippy) to forbid `unsafe` blocks:

```toml
[lints.rust]
unsafe_code = "forbid"
```

- `forbid`: Strongest — `unsafe` code triggers a compile error and cannot be overridden with `allow`
- `deny`: `unsafe` triggers a compile error but can be overridden with `allow` in specific modules
- `warn` / `allow`: Not recommended for firmware where safety is critical

## Memory Allocation and Release

- All runtime states must belong to a certain object instance.
- Avoid creating local variables larger than 128 bytes on task stacks; 
- Pre-allocated buffers, memory pools or ring buffers should be used in high-frequency scenarios.

## Testing

- Firmware changes should at minimum run `idf.py build` for the affected board configuration after exporting ESP-IDF and generating board manager config.
- Component test apps live under `components/claw_modules/*/test_apps/`.
- Lua module tests live beside modules under `components/lua_modules/<module>/test/` with descriptive names such as `json_roundtrip.lua`.
- Embedded frontend changes should run `cd application/edge_agent/components/http_server/frontend_source && pnpm build` and `pnpm typecheck`.

## Common File Locations

- App entry point: `application/edge_agent/main/main.c`
- Capability registration: `components/common/app_claw/app_capabilities.c`
- Lua module registration: `components/common/app_claw/app_lua_modules.c`
- App config schema/storage: `application/edge_agent/components/app_config/`
- Board definitions: `application/edge_agent/boards/`

## AGENTS.md Best-Practice Notes

Use this file as a compact router, not an encyclopedia.

- Keep instructions specific to this repository and this documentation workflow.
- Prefer exact file paths and commands over broad principles.
- Point agents to the right source files instead of duplicating long architecture explanations here.
- Document boundaries and exceptions explicitly, especially when "do not create a page by default" is the expected behavior.
- Update this guide when the docs workflow changes; stale agent docs are worse than missing prose.
