//! Device-only C-ABI scenario runners that drive `claw-api` end to end on real
//! hardware.
//!
//! - **Sync**: [`claw_api_selftest_chat`] builds a `ClawApi` over `EspIdfHttp`
//!   and issues a blocking chat request to a live OpenAI-compatible endpoint.
//! - **Async**: [`claw_api_selftest_chat_async`] builds a `ClawApiAsync` over
//!   `EspIdfHttp` and issues an async chat request through the full API stack.
//!   Driven by `edge-executor`'s `LocalExecutor`.
//!
//! The entire crate is gated on the `espidf` target so a host
//! `cargo build --workspace` compiles it to an empty static archive.
#![cfg(target_os = "espidf")]

use core::ffi::{c_char, c_int};
use core::sync::atomic::AtomicBool;
use core::time::Duration;
use std::ffi::CStr;

use claw_api::{BackendKind, ChatRequest, ClawApi, ClawApiAsync, ClawApiConfig};
use claw_interface::{Cancel, ClawTimer, SleepOutcome, TimerFuture};
use claw_sys::EspIdfHttp;
use serde_json::json;

/// Chat completed and returned text.
const OK: c_int = 0;
/// A required pointer argument was null (or not valid UTF-8).
const ERR_NULL_ARG: c_int = -1;
/// `ClawApi::set_config` rejected the config.
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

#[derive(Default)]
struct NoDelayTimer;

impl ClawTimer for NoDelayTimer {
    fn sleep<'a>(&'a mut self, _duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
        Box::pin(async move {
            if cancel.is_cancelled() {
                SleepOutcome::Cancelled
            } else {
                SleepOutcome::Completed
            }
        })
    }
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

    let config = ClawApiConfig::new(BackendKind::OpenAiCompatible, api_key, model, base_url);

    let Ok(http) = EspIdfHttp::new(base_url) else {
        return ERR_INIT;
    };
    let mut api = ClawApi::new(http);
    if api.set_config(config).is_err() {
        return ERR_INIT;
    }

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

/// Async variant: build a `ClawApiAsync` over the async `esp_http_client` seam
/// and run a chat request. Driven by `edge-executor::LocalExecutor` on the
/// calling thread. Returns [`OK`] on a text reply, or a negative error.
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

    let mut config = ClawApiConfig::new(BackendKind::OpenAiCompatible, api_key, model, base_url);
    config.max_tokens = 64;
    let user_message = user_message.to_string();

    let executor: edge_executor::LocalExecutor = Default::default();
    let out_ptr = out;
    let out_sz = out_len;

    let task = executor.spawn(async move {
        let abort = AtomicBool::new(false);
        let mut api = ClawApiAsync::new(EspIdfHttp::default(), NoDelayTimer);
        if api.set_config(config).is_err() {
            return (ERR_INIT, "failed to initialize async api".to_string());
        }
        let messages = json!([{ "role": "user", "content": user_message }]);
        let request = ChatRequest::new(
            "You are a concise test assistant. Reply in one short sentence.",
            &messages,
        );

        match api.chat(&request, Cancel::new(&abort)).await {
            Ok(response) => match response.text {
                Some(text) => {
                    if text.is_empty() {
                        (ERR_NO_TEXT, "model returned empty content".to_string())
                    } else {
                        (OK, text)
                    }
                }
                None => (ERR_NO_TEXT, "model returned no text".to_string()),
            },
            Err(error) => (ERR_CHAT, error.to_string()),
        }
    });

    let (code, text) = edge_executor::block_on(executor.run(task));
    write_cstr(&text, out_ptr, out_sz);
    code
}
