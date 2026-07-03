# claw-api

LLM client: OpenAI- and Anthropic-compatible chat, structured JSON output, and
image inference over an injected HTTP transport.

Extracted from `claw_core::llm` into a standalone crate so the LLM surface can
be reused independently of the agent core (e.g. by `claw-memory`'s compactor and
the `cap_llm_inspect` capability).

## Entry point

Build a `ClawApi` once from a `ClawApiConfig` plus an HTTP transport (any
`claw_interface::http::ClawHttp`), then issue requests:

| Method | Request | Returns |
|---|---|---|
| `ClawApi::chat` | `ChatRequest` | `LlmResponse` (text + reasoning + tool calls) |
| `ClawApi::chat_json` | `ChatJsonRequest` | `ChatJsonResponse` (parsed `T` + tool calls) |
| `ClawApi::infer_media` | `MediaRequest` | `String` (model text about the image) |

Both `openai_compatible` and `anthropic_compatible` backends are supported; the
crate converts the unified request shape (and tool definitions, tool-call /
tool-result roles, structured-output config) into each provider's wire format.

## Networking is injected

`claw-api` never opens sockets itself. On device the espidf layer implements
`ClawHttp` over `esp_http_client`; tests and host tools provide their own
implementation. This keeps the crate a pure-Rust, host-testable core.

## Cancellation

Every call takes an `&AtomicBool` abort flag. Set it from another thread to
cancel: the transport stops the in-flight request and any retry backoff sleep
returns early. An aborted call surfaces as a non-retryable
`ClawApiError::Transport` whose message contains `"aborted"`.

## Retries

Retry is configured **per call** via `RetryPolicy` on the request (not on the
client). A fresh request carries `RetryPolicy::default()` (2 retries, 500 ms
initial interval, exponential, capped at 8 s); override with `.with_retry(..)`
or disable with `RetryPolicy::none()`. Only transient transport failures are
retried (network errors and HTTP 408/429/5xx); aborts, bad URLs/bodies, and
other 4xx are never retried. See `ClawApiError::is_retryable` for the
classification.

## Public API

Curated re-exports (implementation modules — backend registry, media-prep
pipeline, retry loop — are private):

- Client: `ClawApi`, `ClawApiAsync`
- Config / requests: `ClawApiConfig`, `BackendKind`, `ChatRequest`, `ChatJsonRequest`,
  `MediaRequest`, `RetryPolicy`, `StaticOutputSchema`
- Responses / values: `LlmResponse`, `ChatJsonResponse`, `ToolCall`,
  `MediaAsset`
- Errors: `ClawApiError`, `ChatError`, `ChatJsonError`, `InferMediaError`,
  `InitError`, `ParseBackendKindError`

## Example

```bash
cargo run -p claw-api --example chat --target x86_64-unknown-linux-gnu
```

Builds a client over a stub transport and runs both a plain `chat` (free-form
text) and a `chat_json` (a typed struct parsed and validated against a JSON
schema). The crate-level rustdoc has the same flow with line-by-line commentary.

## Where it fits

A pure-Rust core crate depending on `claw-interface` (the `ClawHttp` seam),
`serde`/`serde_json`, `base64`, and `thiserror`. It is consumed by `claw_core`,
`claw-memory` (default `llm` feature), and LLM-inspection capabilities. The
`#[ignore]`d live integration tests under `tests/` use `claw-interface`'s
`realhttp` transport against a mock endpoint.
