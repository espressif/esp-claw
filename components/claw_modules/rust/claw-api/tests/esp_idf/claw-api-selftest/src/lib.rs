//! Device-only C-ABI scenario runners that drive `claw-api` end to end on real
//! hardware.
//!
//! - **Sync**: [`claw_api_selftest_chat`] builds a `ClawApi` over `EspIdfHttp`
//!   and issues a blocking chat request to a live OpenAI-compatible endpoint.
//! - **Async**: [`claw_api_selftest_chat_async`] constructs the same
//!   OpenAI-format request body and sends it via `ClawHttpAsync`, proving the
//!   async transport works for LLM API calls. Driven by `edge-executor`'s
//!   `LocalExecutor`.
//!
//! The entire crate is gated on the `espidf` target so a host
//! `cargo build --workspace` compiles it to an empty static archive.
#![cfg(target_os = "espidf")]

use core::ffi::{c_char, c_int};
use core::sync::atomic::AtomicBool;
use std::ffi::CStr;

use claw_api::{ChatRequest, ClawApi, ClawApiConfig};
use claw_interface::http::{Cancel, ClawHttpAsync, HttpJsonRequest};
use claw_sys::EspIdfHttp;
use serde_json::json;

/// Chat completed and returned text.
const OK: c_int = 0;
/// A required pointer argument was null (or not valid UTF-8).
const ERR_NULL_ARG: c_int = -1;
/// `ClawApi::init` rejected the config.
const ERR_INIT: c_int = -2;
/// The chat call failed (transport, HTTP, or parse error).
const ERR_CHAT: c_int = -3;
/// The model returned no assistant text (e.g. only tool calls).
const ERR_NO_TEXT: c_int = -4;

/// Borrow a C string as `&str`, or `None` if null / not UTF-8.
///
/// # Safety
/// `ptr` must be null or point to a valid NUL-terminated C string that outlives
/// the returned borrow.
unsafe fn cstr<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    CStr::from_ptr(ptr).to_str().ok()
}

/// Copy `text` into the `out`/`out_len` C buffer, NUL-terminated and truncated
/// to fit. No-op when `out` is null or `out_len` is zero.
///
/// # Safety
/// `out` must be null or point to at least `out_len` writable bytes.
unsafe fn write_cstr(text: &str, out: *mut c_char, out_len: usize) {
    if out.is_null() || out_len == 0 {
        return;
    }
    let bytes = text.as_bytes();
    let copy = core::cmp::min(bytes.len(), out_len - 1);
    core::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), copy);
    *out.add(copy) = 0;
}

/// Build a `ClawApi` over `EspIdfHttp` and issue a single chat request to a live
/// OpenAI-compatible endpoint. Returns [`OK`] on a text reply (written to
/// `out`), or a negative error. On error, the error text is written to `out`.
///
/// # Safety
/// All pointer arguments must be valid C strings; `out` must point to `out_len`
/// writable bytes (or be null to skip the copy).
#[no_mangle]
pub unsafe extern "C" fn claw_api_selftest_chat(
    base_url: *const c_char,
    api_key: *const c_char,
    model: *const c_char,
    user_message: *const c_char,
    out: *mut c_char,
    out_len: usize,
) -> c_int {
    let (Some(base_url), Some(api_key), Some(model), Some(user_message)) = (
        cstr(base_url),
        cstr(api_key),
        cstr(model),
        cstr(user_message),
    ) else {
        return ERR_NULL_ARG;
    };

    let config = ClawApiConfig {
        api_key: Some(api_key.to_string()),
        backend_type: "openai_compatible".to_string(),
        model: Some(model.to_string()),
        base_url: Some(base_url.to_string()),
        supports_tools: false,
        supports_vision: false,
        ..Default::default()
    };

    let Ok(mut api) = ClawApi::init(config, EspIdfHttp::new()) else {
        return ERR_INIT;
    };

    let abort = AtomicBool::new(false);
    let messages = json!([{ "role": "user", "content": user_message }]);
    let request = ChatRequest::new(
        "You are a concise test assistant. Reply in one short sentence.",
        &messages,
    );

    match api.chat(&request, &abort) {
        Ok(response) => match response.text {
            Some(text) => {
                write_cstr(&text, out, out_len);
                OK
            }
            None => ERR_NO_TEXT,
        },
        Err(error) => {
            write_cstr(&error.to_string(), out, out_len);
            ERR_CHAT
        }
    }
}

/// Async variant: build the OpenAI-format chat body, POST it via
/// `ClawHttpAsync` (the async `esp_http_client` seam), and extract the
/// assistant reply. Driven by `edge-executor::LocalExecutor` on the calling
/// thread. Returns [`OK`] on a text reply, or a negative error.
///
/// # Safety
/// All pointer arguments must be valid C strings; `out` must point to `out_len`
/// writable bytes (or be null to skip the copy).
#[no_mangle]
pub unsafe extern "C" fn claw_api_selftest_chat_async(
    base_url: *const c_char,
    api_key: *const c_char,
    model: *const c_char,
    user_message: *const c_char,
    out: *mut c_char,
    out_len: usize,
) -> c_int {
    let (Some(base_url), Some(api_key), Some(model), Some(user_message)) = (
        cstr(base_url),
        cstr(api_key),
        cstr(model),
        cstr(user_message),
    ) else {
        return ERR_NULL_ARG;
    };

    let url = format!("{base_url}/chat/completions");
    let body = json!({
        "model": model,
        "messages": [
            { "role": "system", "content": "You are a concise test assistant. Reply in one short sentence." },
            { "role": "user", "content": user_message }
        ],
        "max_tokens": 64
    })
    .to_string();

    let executor: edge_executor::LocalExecutor = Default::default();
    let out_ptr = out;
    let out_sz = out_len;

    let task = executor.spawn(async move {
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: &body,
            api_key: Some(api_key),
            auth_type: Some("bearer"),
            timeout_ms: 30_000,
            headers: &[],
        };
        let pending = ClawHttpAsync::post_json(&EspIdfHttp::new(), &request, Cancel::new(&abort));
        match pending.await {
            Ok(response) if response.status_code == 200 => {
                let parsed: Result<serde_json::Value, _> = serde_json::from_str(&response.body);
                match parsed {
                    Ok(v) => {
                        let text = v["choices"][0]["message"]["content"].as_str().unwrap_or("");
                        if text.is_empty() {
                            (ERR_NO_TEXT, "model returned empty content".to_string())
                        } else {
                            (OK, text.to_string())
                        }
                    }
                    Err(e) => (ERR_CHAT, format!("json parse: {e}")),
                }
            }
            Ok(response) => (
                ERR_CHAT,
                format!("HTTP {}: {}", response.status_code, response.body),
            ),
            Err(e) => (ERR_CHAT, format!("transport: {e}")),
        }
    });

    let (code, text) = edge_executor::block_on(executor.run(task));
    write_cstr(&text, out_ptr, out_sz);
    code
}
