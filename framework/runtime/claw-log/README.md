# claw-log

Upper-layer logging for the claw firmware: the [`log`] facade backend and a
flat-tree `tracing` subscriber, both driving the same output.

`claw-log` sits above `claw-sys` (which owns the `ESP_LOGx` C bridge) and gives
the rest of the firmware two independent logging streams that share one line
format:

- **`log` facade** — plain `log::error!`/`warn!`/`info!`/… records.
- **`tracing`** — spans/events rendered as a flat tree, with caller-declared
  inherited-context groups and logical async-task lanes.

Both render as ESP-IDF's `<L> (<ms>) <tag>: <msg>` line with ESP-IDF's
per-level colors.

## Where the output goes

| Target | `log` facade | `tracing` |
|---|---|---|
| device (`espidf`) | `claw_sys`'s `ESP_LOGx` bridge | same bridge |
| host | `env_logger` (custom format) | re-enters the `log` facade → `env_logger` |

The two streams are intentionally independent — no `tracing/log-always`, no
`LogTracer` — so a `tracing` event never re-emits as a `log` record (and vice
versa), eliminating any log↔trace loop.

## Public API

| Item | Role |
|---|---|
| `init_logger(max_level, output)` | Install the global `log` backend. `max_level` is the authoritative runtime filter (`RUST_LOG` is **not** consulted). |
| `init_tracing(config)` | Install the flat-tree `tracing` subscriber; only `claw*` targets are traced. |
| `LogOutput` | Sink selection: `Stderr` (default) or `File(path)` (host-only convenience, truncated on open, no color). |
| `TracingConfig` | Declares inherited-context groups via `with_context_group_keys(name, keys)`. Keeps the generic trace layer decoupled from `claw_core`'s concepts. |
| `InitLoggerError` | `init_logger` failure (`OpenLogFile`, `SetLogger`). |
| `LevelFilter` | Re-exported from `log`. |
| `FlatTreeSubscriber` / `TraceSink` | The subscriber and its output seam (from the `trace` module). |

An independently scheduled future opens a stable logical lane by adding
`trace.task = %task_id` to its root span. `FlatTreeSubscriber` consumes that
reserved field and propagates the label to descendant spans, events, and the
closing record; physical thread names are only the fallback for synchronous
work outside such a span. See [the trace format](docs/trace-format.md) for the
full contract.

The runtime registers `run` as
`system → session → turn → agent → iteration`. `system` is the outer
agent-system scope used by startup/global work before any session exists; it is
a fixed semantic label rather than an allocated id. Session and agent futures
then open their own logical task lanes.
The Python [Chrome exporter](scripts/README.md) maps those scopes to synthetic
Chrome processes. A session requires its inherited system scope; the exporter
does not accept the older session-without-system shape. Records outside both a
system and a session remain valid and use `unattributed`.

Chrome counters are explicit: only `counter.<series>=<number>` event fields
produce `ph=C`. Numeric lifecycle metadata remains on its instant event as
ordinary arguments.

## Level ceilings

Two layers cap verbosity:

- **Compile-time** (`Cargo.toml` features): `log_max_*` forwards to
  `log/release_max_level_*` and `trace_max_*` to `tracing/release_max_level_*`,
  stripping higher-level macros out of **release** builds entirely (no
  formatting, no FFI). Exactly one `log_max_*` may be enabled at a time. Default
  is `log_max_info` (mirrors ESP-IDF's `CONFIG_LOG_MAXIMUM_LEVEL`).
- **Runtime**: `init_logger`'s `max_level` argument, plus ESP-IDF's
  `esp_log_level_set` / `CONFIG_LOG_DEFAULT_LEVEL` on device. On host, noisy
  dependencies (reqwest/rustls/…) are capped at `Warn` regardless, so
  `init_logger(Trace)` keeps first-party verbosity without the dependency flood.

## Example

```bash
cargo run --example fmt_demo -p claw-log
```

Emits one line per source (`log` facade and `tracing`) so you can eyeball the
unified format. Piped (non-TTY) output is plain text (ANSI auto-stripped); on a
TTY it shows ESP-IDF per-level colors.

## Notes

`env_logger` is a host-only dependency (`cfg(not(target_os = "espidf"))`); the
device build never pulls it in. The `scripts/` directory holds a serial-log
helper for reading device output.
