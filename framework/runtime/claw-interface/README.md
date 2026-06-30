# claw-interface

The OS / platform abstraction layer for the claw Rust crates.

This is the **inbound boundary** (C / OS → Rust): it defines the
**dependency-injection traits** that abstract over platform facilities —
filesystem (`ClawFs`) and networking (`ClawHttp`) — plus the **shared types**
those traits work with. The pure-Rust core crates (`claw-api`, `claw_core`,
`claw-capability`, `claw-memory`, `claw-sandbox`, …) depend only on these traits, never
on a platform directly, so the device build and host tests can plug in different
implementations of the same seam.

## What's here

### `fs` — the `ClawFs` persistence seam

The byte-oriented filesystem injection point for everything that must survive a
reboot (conversation tapes, profile/long-term memory, …). Two write disciplines
coexist: `append` + `read_at` for append-only journals, and `write_atomic` for
tear-free whole-file checkpoints.

| Item | Role |
|---|---|
| `ClawFs` | The trait: `read`, `read_at`, `len`, `write_atomic`, `append`, `create_dir_all`, `exists`, `remove`, `list_dir`. |
| `FsError` | Coarse failure: `NotFound` vs `Io(..)`. |

### `http` — the `ClawHttp` networking seam

The JSON-POST injection point for the LLM backends, plus an async variant.

| Item | Role |
|---|---|
| `ClawHttp` | Blocking `post_json(request, abort)` → `HttpResponse`. |
| `ClawHttpAsync` / `HttpResponseFuture` / `Cancel` | Object-safe async transport with structural cancellation (drops the in-flight transfer when the abort flag is set). |
| `HttpJsonRequest` / `HttpHeader` / `HttpResponse` | Request/response shapes (mirror the C transport structs). |
| `HttpError` | Transport failure (`Aborted`, `InvalidUrl`, `RequestFailed`, `UnexpectedStatus`, …). |

## Host-only reference implementations (opt-in features)

These live beside the traits only to keep the few distinct implementations in
one place. They are **never** enabled in a device build.

| Feature | Provides |
|---|---|
| `memfs` | `MemFs` — an in-memory `ClawFs` test double (no extra deps). |
| `diskfs` | `DiskFs` — a `std::fs`-backed `ClawFs` for host CLIs and disk tests. |
| `diskfs-pretty` | `DiskFs` that pretty-prints `.json` writes (implies `diskfs`). |
| `httpmock` | Scripted / capturing / failing / never-called `ClawHttp` doubles: `ScriptedHttp`, `CapturingHttp`, `FailingHttp`, `NeverHttp`, `NoopHttp`, `YieldingClawHttpAsync`, `BlockingClawHttpAsync`, `ScriptStep`. |
| `realhttp` | `RealHttp` / `RealHttpAsync` — a blocking reqwest backend for host CLIs and live tests. |

## Example

```bash
cargo run -p claw-interface --example di_seams \
    --features memfs,httpmock --target x86_64-unknown-linux-gnu
```

Exercises both seams with host doubles: a `MemFs` for the `ClawFs` operations
the modules rely on, and a `ScriptedHttp` serving canned LLM replies through the
`ClawHttp` trait. (The example declares these as `required-features`.)

## Where it fits

Everything downstream — `claw-api`, `claw_core`, `claw-capability`, `claw-memory`,
`claw-sandbox`, … — depends on this crate's traits and types and stays
platform-agnostic. The on-device implementations live in `claw-sys` (e.g.
`EspIdfHttp`) and the firmware wiring; the host implementations are the
feature-gated doubles above.
