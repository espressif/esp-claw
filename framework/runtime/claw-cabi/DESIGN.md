# claw-cabi — design

`claw-cabi` is the **single outbound C ABI** for the Rust agent/capability stack:
the one crate that turns the pure-Rust APIs back into a C ABI for the firmware's
C callers. It is the only crate allowed to contain `unsafe` / `#[no_mangle]
extern "C"`; every other Rust crate keeps `unsafe_code = "forbid"`.

Scope is deliberately tiny — three functions total:
- **control plane**: register capabilities (`claw_capability_register` /
  `claw_capability_register_group`, §5);
- **data plane**: inbound delivery (`claw_capability_ingress_push`, §5b) — the
  receive half of a channel, mirroring `Orchestrator::push_user_message`.

Lifecycle driving, queries, and *building* the agent runtime are owned and
driven from Rust, not exposed to C. The crate also contains the Rust-side bridge
that wires the populated `Registry` into the agent (§6) — that is plain Rust,
not ABI.

> Status: **the three ABI functions are implemented and host-tested**
> (`src/result.rs`, `src/abi.rs`, `src/wrappers.rs`, `src/lib.rs`).
> `include/claw_cabi.h` is the **authoritative, hand-maintained** header — its
> layout and enum discriminants match the Rust exactly. cbindgen is kept only as
> a layout cross-check (`cbindgen.toml`), not the generator of record: it strips
> the header prose and emits enum constants as `CLAW_CAPABILITY_ERROR_KIND_T_OK`
> (a `_t`-qualified prefix) rather than the agreed `CLAW_CAPABILITY_OK` names.
>
> The §6 bridge **glue lives in `claw-agent`** (`src/capability.rs`:
> `RegistryResolver` (tools -> `AgentResolver`, skills threaded through),
> `RegistryChannelTransport` (channel adapter -> `ChannelTransport`), and
> `register_channels`) and is wired automatically by
> `AgentSystemBuilder::capabilities(...)`. `claw-cabi` **consumes claw-agent's
> wrapped API** rather than re-wrapping the lower crates: it depends only on
> `claw-agent` (with `default-features = false` so the dev backends never reach
> the device image) and uses its re-exports. Still pending: the **firmware Rust
> entry point** that builds an `AgentSystem` from the populated `Registry`
> (`builder().capabilities(registry)`), drives lifecycle, and hands the
> orchestrator out as the `claw_capability_ingress_t` via `AgentSystem::ingress()`
> — app-shell wiring against `claw-agent`'s construction API.

Inbound C→Rust shims (`claw-sys`: log sink, HTTP, thread) are the *opposite*
direction and stay where they are. This crate is strictly outbound (Rust→C).

---

## 1. Guiding decisions

1. **C's surface is the two data/control planes, nothing more.**
   - **Control plane** — register capabilities: `claw_capability_register` +
     `claw_capability_register_group` (§5).
   - **Data plane** — the inbound half of a channel: `claw_capability_ingress_push`
     (§5b). This is *required*, not optional: a channel is bidirectional, and
     this is the exact mirror of Rust's `Orchestrator::push_user_message`
     (outbound is the descriptor's `send` callback). Without it a C channel
     could transmit but never receive.

   Everything else — lifecycle *driving* (`start_all` / `enable_group` / …),
   queries, and *building* the agent runtime — is driven from **Rust** and is
   not exposed to C. The registry and the ingress handle are both created and
   owned Rust-side; handles are passed to C purely as a registration target and
   an inbound port.
2. **C sees only "capability".** No `tool` / `channel` vocabulary crosses the
   ABI as a separate concept. There is one descriptor `claw_capability_t` whose
   role (Tool / Channel / lifecycle-only) is a **tagged union** selected by a
   `role` discriminant — a faithful structural mirror of the Rust
   `CapabilityRole` enum, so the arms are mutually exclusive by construction
   (see §4).
3. **`Result`-shaped returns.** The two registration functions return
   `claw_capability_result_t` = either `OK` (no payload) or an **error kind + message**
   (see §3).
4. **No string ownership crosses the ABI — borrowed both ways** (see §3). Every
   `message` is a `const char*` the *producer* keeps owning; the *receiver*
   copies it if it wants to keep it and **never frees** it. There is no
   `*_free` function. This is the consistent realization of "copy, don't
   transfer": Rust stays idiomatic, and no allocator is shared across the
   boundary.
5. **No dispatch / visibility / session / catalog.** Those legacy `claw_cap`
   surfaces are dropped (see §7); tool invocation and LLM visibility live in
   `claw-tool` / `claw-core`.

---

## 2. The registry handle & lifetime

```c
typedef struct claw_capability_registry claw_capability_registry_t;  // wraps Arc<claw_capability::Registry>
typedef struct claw_capability_ingress  claw_capability_ingress_t;   // wraps Arc<dyn claw_core::ChannelIngressSink>
```

- Both handles are **created and destroyed on the Rust side**; C never creates
  or frees either. The Rust entry point hands the registry to C so C can
  register into it, and (after the runtime is wired) hands the ingress so C
  channel gateways can push received messages in.
- Every handle/pointer parameter is null-checked → `CLAW_CAPABILITY_INVALID_ARGUMENT`.
- All `const char*` inputs are **copied into owned `String`s during the call**
  (UTF-8 validated; invalid → `CLAW_CAPABILITY_INVALID_ARGUMENT`). The ABI never
  retains an input pointer past the call that received it.
- `user_context` is owned by C and must outlive the capability (until its
  `deinit` has run). Documented contract.
- Underlying behavior objects (the C callbacks wrapped as `ToolHandler` /
  `ChannelAdapter` / `Lifecycle`) are shared via `Arc`; the `Registry` owns
  identity + lifecycle state, and `tools()` / `channels()` hand out `Arc` clones
  to the Rust-side bridge (§6).

---

## 3. `Result`-shaped return — borrowed message, no free

```c
typedef enum {
  CLAW_CAPABILITY_OK = 0,            // success; message == NULL
  CLAW_CAPABILITY_INVALID_ARGUMENT,
  CLAW_CAPABILITY_NOT_FOUND,
  CLAW_CAPABILITY_ALREADY_EXISTS,
  CLAW_CAPABILITY_INVALID_STATE,
  CLAW_CAPABILITY_FAILED,            // catch-all incl. panic guard / callback-reported failure
} claw_capability_error_kind_t;

typedef struct {
  claw_capability_error_kind_t kind;
  const char* message;               // OK => NULL; error => BORROWED (never free it)
} claw_capability_result_t;

static inline bool claw_capability_is_ok(claw_capability_result_t result) { return result.kind == CLAW_CAPABILITY_OK; }
```

There is **no `*_free` function**. `message` is always borrowed; the receiver
copies it if it needs to outlive the validity window, and never frees it.

**Rust → C** (return value of the `claw_capability_register*` / `claw_capability_ingress_push` functions):

- Structural kinds (`INVALID_ARGUMENT`, `NOT_FOUND`, …) point at `'static` strings.
- `FAILED`'s dynamic text lives in a Rust **thread-local "last error" buffer**,
  valid **until the next `claw-cabi` call on the same thread**. C must read/copy
  it before its next call into this ABI (errno-style). Rust keeps ownership.

**C → Rust** (return value of the callbacks in §4): `message` is borrowed from C.
Rust copies it into `CapabilityError::Failed(String)` *at the synchronous point
the callback returns* and never frees it. C keeps ownership and only guarantees
the pointer is valid until the callback returns — a string literal, a `static`
buffer, or one hung off `user_context` all satisfy this.

Rationale: Rust stays idiomatic (`CStr` copy on both sides); no allocator is
shared across the boundary; the error path performs no per-call heap allocation
in C and there is no "forgot to free" leak risk anywhere.

---

## 4. Capability descriptor — a tagged union over the role

The internal `CapabilityRole` enum (`Tool` / `Channel` / `None`) is mirrored
**structurally** as a C tagged union: a `role` discriminant + a union of the
mutually exclusive payloads. "Both Tool and Channel" is therefore impossible to
express (the arms share storage), instead of being a runtime-rejected mistake.
`lifecycle` and `user_context` are orthogonal common fields and stay outside the
union — matching the Rust `Capability { id, description, role, lifecycle }`.

```c
typedef claw_capability_result_t (*claw_capability_lifecycle_callback_t)(void* user_context);
typedef struct { claw_capability_lifecycle_callback_t init, start, stop, deinit; } claw_capability_lifecycle_t;

typedef claw_capability_result_t (*claw_capability_execute_callback_t)(
    const char* arguments_json,
    char* output_buffer, size_t output_capacity, size_t* output_length, bool* output_success,
    void* user_context);

typedef claw_capability_result_t (*claw_capability_send_callback_t)(
    const char* channel, const char* chat_id, const char* text,
    const char* reply_to_message_id /* nullable */, void* user_context);

typedef enum {
  CLAW_CAPABILITY_ROLE_NONE = 0,   // lifecycle-only service; no payload
  CLAW_CAPABILITY_ROLE_TOOL,       // model-callable tool
  CLAW_CAPABILITY_ROLE_CHANNEL,    // message channel
} claw_capability_role_t;

typedef struct { const char* schema_json; claw_capability_execute_callback_t execute; } claw_capability_tool_t;
typedef struct { claw_capability_send_callback_t send; } claw_capability_channel_t;

typedef struct {
  const char*            id;          // required, unique
  const char*            description; // nullable
  claw_capability_role_t role;        // selects the live union arm
  union {
    claw_capability_tool_t    tool;     // role == ROLE_TOOL
    claw_capability_channel_t channel;  // role == ROLE_CHANNEL
  } role_data;                          // unused when role == ROLE_NONE
  claw_capability_lifecycle_t lifecycle; // orthogonal; the four hooks may each be NULL
  void* user_context;                    // passed to every callback above
} claw_capability_t;
```

The Rust side defines this as a `#[repr(C)]` struct with a `#[repr(C)]` union +
the `role` tag; reading the union is `unsafe` (allowed only in this crate) and
gated on `role`.

**Validation** (all violations → `INVALID_ARGUMENT`):

| `role` | required payload | else |
|--------|------------------|------|
| `ROLE_TOOL` | `role_data.tool.execute` **and** `.schema_json` non-NULL | INVALID_ARGUMENT |
| `ROLE_CHANNEL` | `role_data.channel.send` non-NULL | INVALID_ARGUMENT |
| `ROLE_NONE` | at least one `lifecycle` hook set | INVALID_ARGUMENT (does nothing) |
| any | non-empty `id` | INVALID_ARGUMENT |

**Tool output buffer.** Rust passes a buffer of a named capacity
`CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY`; the C `execute` writes its result there,
sets `*output_length` to bytes written and `*output_success` to the tool's
success flag. If the output would exceed the capacity, `execute` returns
`CLAW_CAPABILITY_FAILED` with a message (default: fixed buffer; two-call size
negotiation considered and rejected for simplicity — revisit if real tools
exceed the capacity).

**Concurrency is Rust-only, by design.** `ToolHandler::concurrent()` (the async-
runner hint) is **not** exposed on the C descriptor: every C-registered tool is
always serial (`concurrent() == false`). Only tools registered directly in Rust
(as a `claw_tool::Tool`) can opt into concurrent execution. The C ABI stays
minimal and C capabilities never have to reason about batch concurrency.

```c
typedef struct {
  const char* id;                            // group id (provenance + enable/disable handle)
  const claw_capability_t* members;
  size_t member_count;
  claw_capability_lifecycle_t lifecycle;     // shared group lifecycle; hooks may be NULL
  void* user_context;
} claw_capability_group_t;
```

---

## 5. Control plane: registration

```c
claw_capability_result_t claw_capability_register      (claw_capability_registry_t*, const claw_capability_t*);
claw_capability_result_t claw_capability_register_group(claw_capability_registry_t*, const claw_capability_group_t*);
```

Both map onto `claw_capability::Registry::register` / `register_group`.
Re-registering an existing id returns `ALREADY_EXISTS` (so an idempotent caller
just ignores that kind — no separate `group_exists` query is exposed).

## 5b. Data plane: inbound delivery

```c
typedef struct {
  const char* message_id;
  const char* channel;
  const char* chat_id;
  const char* sender_id;   // nullable
  const char* session_id;
  const char* text;
} claw_inbound_message_t;

claw_capability_result_t claw_capability_ingress_push(claw_capability_ingress_t*, const claw_inbound_message_t*);
```

This is the **receive half of a channel** and the exact mirror of Rust's
`Orchestrator::push_user_message(InboundMessage)` (`Orchestrator` implements
`claw_core::ChannelIngressSink`; `claw_inbound_message_t` maps field-for-field onto
`claw_core::InboundMessage`). The reply is **not** returned here — it flows back
out asynchronously through the target channel's `send` callback. A C channel
gateway (usually a task spawned in the channel capability's `start` hook) calls
this when a message arrives.

`push_command` (`InboundCommand` = preempt/etc.) is **not** exposed: those are
internal/CLI control signals, not something a C channel produces.

**Everything else stays Rust-side, not in this ABI:**

- **Lifecycle driving** — `start_all` / `stop_all` / `enable_group` /
  `disable_group` / `unregister_group` / `unregister` are called by the Rust
  entry point that owns the `Registry`, not by C. The lifecycle *behavior* C
  supplies still runs (the `init`/`start`/`stop`/`deinit` callbacks in the
  descriptor); Rust just decides *when*. Order is the Rust crate's: enable runs
  `group.init?→group.start→members(init?→start)`, disable runs members' then
  group's `stop` (reverse), and `unregister` additionally runs the one-time
  `deinit` (members then group, only where `init` ran).
- **Queries** (`group_exists` / `contains` / `group_state` / `state_of`) and the
  **role accessors** (`tools` / `channels`) are Rust-only.

---

## 6. Bridge: Registry → agent runtime (Rust wiring)

C fills the `Registry` (§5) and later pushes inbound (§5b). *Building* the agent
runtime and wiring it to the registry happens entirely in **Rust** — the
firmware's Rust entry point owns the `Registry` and the `Orchestrator`, drives
lifecycle (`start_all`, …), and wires the two together. The wiring itself is
plain Rust glue in **`claw-agent`** (`src/capability.rs`), applied through
`AgentSystemBuilder::capabilities(...)`; `claw-cabi` does not duplicate it. The
only C entries on the message path are the `send` callback (out) and
`claw_capability_ingress_push` (in).

- **tools → resolver.** `RegistryResolver { registry: Arc<Registry> }:
  AgentResolver` in **claw-agent**: `resolve_tool(name) = registry.tool(name)`
  (disabled/absent → `None` → manifest build fails with `UnknownCapability`,
  never silently dropped). Skills stay on `claw-skill`'s `SkillRegistry`,
  unrelated to capabilities.
- **channels → egress.** For each `registry.channels()` adapter, register a
  `ChannelTransport` into the `ChannelEgressHub`. A small adapter converts
  `claw_capability::OutboundMessage` ↔ `claw_core::channels::OutboundMessage`
  **in this bridge** (so `claw-capability` does not depend upward on
  `claw-core` — open item H1, decided: convert at the bridge).
- **outbound** replies flow out through the registered channel's C `send`
  callback (Rust → C; a *callback* in the descriptor, not an ABI function).
- **inbound** received messages flow in through `claw_capability_ingress_push`
  (§5b, C → Rust): the bridge builds the `Orchestrator` (which is the
  `ChannelIngressSink`), wraps `Arc<dyn ChannelIngressSink>` in a
  `claw_capability_ingress_t`, and the Rust entry point hands that to C. So the
  message path is closed on both ends: C gateway → `ingress_push` → `Orchestrator` →
  egress → channel `send` → C gateway.

---

## 7. Dropped from the legacy `claw_cap.h`

Not exposed by this ABI (handled elsewhere or removed):

- `kind` / `cap_flags` (`CALLABLE_BY_LLM`, `EMITS_EVENTS`, `SUPPORTS_LIFECYCLE`,
  `RESTRICTED`, `ROOT_AGENT_ONLY`), `name` (dup of `id`), `family`.
- `claw_cap_call_context_t` and all its fields; `caller`; `core` handle.
- `claw_cap_call` / `claw_cap_call_from_core` (dispatch → `claw-tool` / agent loop).
- `set_llm_visible_groups` / `set_session_llm_visible_groups` (visibility →
  `claw-core` `ToolSet` composition).
- `build_llm_tools_json` / `build_catalog` / `*_tools_provider` (→ `claw-core`).
- `claw_cap_find` / `claw_cap_list` / `claw_cap_list_groups` /
  `descriptor_info.active_calls`.
- `STATE_DRAINING` / `STATE_UNLOADING` (no in-flight dispatch to drain in this
  layer).

---

## 8. Safety contract (rustdoc + header comments)

1. **No unwinding across FFI.** Every `extern "C"` body is wrapped in
   `catch_unwind`; a panic becomes `CLAW_CAPABILITY_FAILED`. (Device release profile is
   `panic = abort`; `catch_unwind` matters on host/tests.)
2. **Thread-safety.** Callback wrappers hold raw fn pointers + `user_context` and need
   `unsafe impl Send + Sync`; the contract "C callbacks must be thread-safe" is
   documented (the `Registry` is driven from multiple tasks).
3. **UTF-8 + null checks** on every input; violations → `INVALID_ARG`, never a
   silent fallback.

---

## 9. C caller migration impact

Surveyed all 16 components under `framework/capabilities/`. **The common
path maps 1:1** — every component registers via `claw_cap_register_group` with
descriptors carrying `id` / `description` / schema / `execute` (+ optional
`init`/`start`/`stop`). It is **not** a "nothing connects" rewrite.

**Mechanical churn (touches all 16, low-risk, scriptable):**

- Drop dead descriptor fields `.name` / `.family` / `.kind` / `.cap_flags`.
- Set `.role` and move the role payload under `.role_data`: a tool sets
  `.role = ROLE_TOOL` with `.role_data.tool.execute` + `.role_data.tool.schema_json`
  (renamed from `.input_schema_json`); a channel sets `.role = ROLE_CHANNEL` with
  `.role_data.channel.send`.
- **`execute` signature** (the bulk): `esp_err_t fn(input_json, ctx, output,
  output_size)` → `claw_capability_result_t fn(arguments_json, output_buffer,
  output_capacity, *output_length, *output_success, user_context)`. Each tool
  function sets `*output_length`/`*output_success` and returns a result. Note
  the mapping for the common "write `Error: …` into output and return `ESP_OK`"
  idiom → result `OK` with `*output_success = false`.
- Group struct `claw_cap_group_t` → `claw_capability_group_t` (rename + lifecycle
  moved into a struct).
- Lifecycle signature `esp_err_t (*)(void)` → `claw_capability_result_t (*)(void*
  user_context)`.
- **Idempotent registration**: the `claw_cap_group_exists(id)` guard (e.g.
  `cap_time`) goes away — `register_group` returns `ALREADY_EXISTS`, which an
  idempotent caller treats as success.
- **No `start_all` / lifecycle calls in C**: whoever drove `claw_cap_start_all`
  (the app shell) no longer does — the Rust entry point drives lifecycle.

**Semantic changes (only 4 spots, need real porting):**

1. **`ctx->core`** (`cap_llm_inspect` → `claw_core_llm_infer_media`): capture the
   needed handle in `user_context` at registration instead of receiving it per-call.
2. **`ctx->session_id` + per-session visibility** (`cap_skill_mgr`): this is the
   session/visibility we deliberately moved out of the cap layer. Skill
   activation re-homes to `claw-skill` / `claw-core`; this component likely
   changes shape or stops being a capability.
3. **IM send reading `ctx->chat_id` / `ctx->channel`** (`cap_im_tg`/`qq`/`feishu`/
   `local`): reconceive the `*_send_message` tool as a **channel `send`** egress —
   `channel`/`chat_id` become explicit `send` parameters (the data is still
   there, just delivered explicitly).
4. **`claw_cap_call`** in 5 `cmd_*.c` CLI commands (`mcp_client`, `web_search`,
   `skill`, `router_manager`, `llm_inspect`): the ABI drops dispatch. These need a
   system-invoke path or to be re-pointed through `claw-tool` / the agent.

**Confirmed unused by callers (safe to drop):** `claw_cap_call_from_core`,
`claw_cap_find`, `claw_cap_list`, `build_llm_tools_json`, `build_catalog`,
`*_tools_provider` — only consumed by `claw_core`-side code, not by the 16
components.

This is all C-side work and stays **postponed**; captured here for when it lands.

## 10. Open items (defaults chosen, revisit on review)

- **H1 `OutboundMessage` unification** → convert at the bridge (above). Keeps
  `claw-capability` free of an upward dependency.
- **Inbound ingestion for C channels** → settled (§5b/§6): the
  `claw_capability_ingress_t` handle + `claw_capability_ingress_push`, mirroring
  `Orchestrator::push_user_message`. Open sub-question is only *how the C app
  threads the ingress handle to its gateway tasks* (handed at boot after wiring;
  stored in the channel's `user_context` or an app global) — a C-side wiring
  detail, not an ABI gap.
- **`RegistryResolver` location** → `claw-agent` (`src/capability.rs`).
- **Tool output buffer** → fixed `CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY` buffer.
- **Crate setup** → `crate-type = ["staticlib", "rlib"]`, `unsafe_code = "allow"`
  (the only such crate), clippy panic lints still `deny`, depends on `claw-agent`
  alone (`default-features = false`, so the dev backends — `DiskFs` / reqwest
  `RealHttp` / `StdThread` — are not compiled into the device image); the header
  is hand-maintained with cbindgen as a layout cross-check.
