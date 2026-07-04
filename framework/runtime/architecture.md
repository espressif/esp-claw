# Architecture

## Crates

`claw-interface`: defines the inbound DI traits (`ClawFs`, `ClawHttp`, `ClawThread`, `ClawTimer`) and shared types. does not directly implement impls for any target.
`claw-sys`: implements `claw-interface` for `espidf` targets — the `ESP_LOGx` log sink (C↔Rust logging bridge) and the `esp_http_client` `ClawHttp` driver. linux/host impls plug into the same traits for tests.
`claw-utils`: shared leaf utilities — log-safe text truncation, the `define_prefixed_id!` / `define_id_allocator!` newtype macros, async channel helpers, and the small host/test `block_on` executor.
`claw-log`: upper-layer logging — the `log` facade backend and flat-tree `tracing` subscriber that write through `claw-sys`'s `ESP_LOGx` sink, plus compile-time `log_max_*` / `trace_max_*` level ceilings for stripping log/trace calls from release builds.
`claw-api`: llm unified api for different llm backends, uses `claw-interface`. does not directly touch the specific trait impls.
`claw-permission`: permission policy — classifies tool actions into `Allow` / `Ask` / `Deny` decisions. no platform deps.
`claw-sandbox`: sandbox filesystem — wraps a `ClawFs` and confines agent file access to a fixed set of virtual roots (`/sandbox`, `/shared`, `/system`), rejecting any path outside them.
`claw-capability`: capability registry and tool runtime — `Tool` / `ToolSet`, central enable/disable, tool execution, and the build-time bake contract for embedding tool metadata assets at compile time.
`claw-context`: context assembler — owns placement, change detection, and rendering of `Block`s into a `RequestContext` for the LLM client. content is supplied by callers; this crate never fetches memory, history, or skills itself.
`claw-memory`: agent memory — three pure-storage stores: `TranscriptStore` (append-only verbatim conversation record), `ProfileStore` (editable profile documents), and `LongTermMemory` (durable fact store). defines the `Compactor` seam but does no compaction itself — the LLM-backed compactor lives in `claw-core`.
`claw-skill`: skill registry — scans `SKILL.md` skill files over a `ClawFs`, renders the catalog, and assembles loaded skill documents into prompt context blocks.
`claw-capability`: capability adapter — maps registered `Capability` items into internal runtime projections with an orthogonal enable/disable lifecycle.
`claw-core`: runtime core — orchestrator shell, session/channel management, and the per-iteration LLM + tool-call loop. uses all the domain crates and `claw-api` but depends on no platform crate directly.
`claw-agent`: agent system api — the public entry point above `claw-core`: builds an `AgentSystem`, wires the `Registry` of capabilities, and drives registered channels. the `dev` feature enables a host-target repl binary (`claw-agent-chat`).
`claw-cabi`: outbound C ABI boundary — the single `extern "C"` layer (Rust → C): C registers capabilities and pushes inbound messages; the agent runtime and all business logic stay on the Rust side. the only crate in the workspace where `unsafe_code = "allow"`.
`cli` (`claw-agent-cli`): host CLI — drives claw agents against a live LLM with on-disk memory for off-device manual testing. not linked into firmware.
