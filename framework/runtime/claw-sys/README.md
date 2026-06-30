# claw-sys

Thin shims to C / ESP-IDF facilities that Rust `std` cannot express on its own.

This is an **inbound boundary crate** (C / OS → Rust): it wraps a handful of
ESP-IDF C APIs in safe Rust and plugs them into the dependency-injection traits
from `claw-interface`, so the pure-Rust core crates depend only on those traits
and never touch C directly. The platform-specific FFI lives behind
`#[cfg(target_os = "espidf")]`; the pure-Rust helpers stay host-testable.

## Modules

### `http` — the `esp_http_client` driver

`EspIdfHttp` implements `claw_interface::http::ClawHttp` over ESP-IDF's
`esp_http_client`, porting the C `claw_llm_http_transport.c`. It is exported only
on the `espidf` target:

```rust
#[cfg(target_os = "espidf")]
pub use claw_sys::EspIdfHttp;
```

The non-FFI logic — auth-header construction (`Bearer` vs `X-API-Key` vs
`none`) and error-body parsing (prefer `error.message`, then top-level
`message`, else a truncated body echo) — is plain Rust and is unit-tested on the
host (see the tests in `src/lib.rs`).

### `log_sink` — the `ESP_LOGx` bridge

`write(level, tag, msg)` forwards one already-formatted line to ESP-IDF's
`ESP_LOGx` macros via per-level C shims (`claw_rs_log_<level>` in
`csrc/claw_rs_log.c`), yielding ESP-IDF's standard timestamp/level/tag
formatting and runtime level filtering. Device-only.

This is just the device-side half of the sink. The upper layer that drives it —
the `log` facade backend, the flat-tree `tracing` subscriber, and the
target dispatch that falls back to host `stderr` — lives in the `claw-log` crate.

### `thread` — worker spawning

`EspIdfThread` is this crate's only export here: the device implementation of
`claw_interface::ClawThread`, mirroring the C `claw_task` policy. It applies the
requested stack size, `Priority`, `CoreAffinity`, and a **PSRAM-backed stack**
(when PSRAM is present) to the next `pthread_create` via `esp_pthread`, then
restores the prior config so unrelated spawns are unaffected. It is a zero-sized
type, so injecting it as a `T: ClawThread` is free. The trait and the
platform-neutral `Priority` / `CoreAffinity` types live in `claw-interface`
(import them from there, not from here); the host implementation is
`claw_interface::StdThread`. Only the concrete FreeRTOS priority numbers and the
`tskNO_AFFINITY` sentinel are espidf details, and they stay private inside this
crate. The wiring layer injects `EspIdfThread` on device / `StdThread` on host.

## Why this is a separate crate

Keeping these shims at the boundary lets the rest of the workspace stay pure
Rust:

- Core crates (`claw_core`, `claw-capability`, `claw-memory`, …) depend on the
  `ClawHttp` / logging traits, not on `esp_http_client` or `ESP_LOGx`.
- Tests inject host implementations of the same traits, so core logic runs
  off-device.
- The `unsafe` FFI surface is small, localized here, and gated to the `espidf`
  target.

## Dependencies

`claw-interface` (the shared traits/types), `log` (the `Level` vocabulary, used
only on the device path), and `serde_json` (HTTP error-body parsing).
