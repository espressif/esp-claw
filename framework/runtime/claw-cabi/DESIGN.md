# claw-cabi design

`claw-cabi` is the outbound C ABI for the Rust runtime. It does not define the
agent model and it does not extend `claw-api`; it adapts the Rust-native runtime
to the firmware's C call shape.

## Role

- C registers capabilities into a Rust-owned `Registry`.
- C channel capabilities receive a `claw_channel_runtime_t` in `open` and push
  inbound messages through that runtime.
- On ESP-IDF, C creates and drives an opaque `claw_agent_system_t`.

The ESP-IDF agent-system functions are an ABI adapter. They assemble the device
backends (`EspIdfFs`, `EspIdfHttp`, `EspIdfTimer`, `EspIdfThread`), start one
worker running `edge_executor`, and translate synchronous C calls into commands
for the async Rust `AgentSystem`.

Host/dev callers should use `claw-agent` directly. They should not go through a
C-style worker adapter and should not need an executor wrapper trait.

## Boundaries

- `claw-agent` owns the Rust API: build an `AgentSystem`, create/delete/list
  sessions, and drive messages asynchronously.
- `claw-cabi` owns pointer validation, C string parsing/copying, opaque handles,
  panic guards, and the ESP-IDF async-to-C worker adapter.
- `claw-api` owns LLM endpoint/request configuration:
  `BackendKind`, API key, model, base URL, timeout, max tokens, and image byte
  limit. CABI passes those fields through; it does not invent model profile
  override layers.

## Agent System Config

`claw_agent_system_config_t` intentionally carries only the C-provided endpoint
tuple plus the agent persistence root:

- `api_key`
- `backend_type`
- `model`
- `base_url`
- `persistence_dir`

`ClawApiConfig::new` owns the Rust defaults for timeout, max tokens, and image
byte limit. C does not pass those values through the agent-system ABI.

All session lifecycle is explicit. C creates a session with
`claw_agent_system_session_create`, binds it to a channel chat with
`claw_agent_system_session_bind`, enumerates live sessions with
`claw_agent_system_session_list`, and deletes a session with
`claw_agent_system_session_delete`.

Message delivery uses `claw_channel_runtime_push` from a registered channel's
`open` callback, or `claw_agent_system_push_message` for direct C-side helper
wiring. CABI validates required C strings; the channel router owns
chat-to-session bindings and reply routing. Unbound `(channel, chat_id)` messages
are rejected; there is no implicit session creation. There is no separate
message-sink object because that would duplicate the channel capability contract.
