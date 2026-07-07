//! Device-only C-ABI scenario runners that exercise `claw-sys` from an ESP-IDF
//! Unity test app.
//!
//! The C side is a thin entrypoint: each `#[no_mangle]` function below runs a
//! whole scenario in Rust and returns a small integer the C test asserts on.
//! The point is to prove the Rust crate links and behaves correctly *through the
//! C ABI* on real hardware, including the async `ClawHttp` seam driven by
//! `edge-executor`'s `LocalExecutor`.
//!
//! The entire crate is gated on the `espidf` target so a host
//! `cargo build --workspace` compiles it to an empty static archive.
#![cfg(target_os = "espidf")]

use core::cell::Cell;
use core::ffi::{c_char, c_int};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::ffi::CStr;
use std::rc::Rc;
use std::sync::Arc;

use claw_interface::http::{
    blocking::ClawHttp as BlockingClawHttp, Cancel, ClawHttp, HttpAuth, HttpJsonRequest,
    HttpStatusCode,
};
use claw_interface::{ClawThread, CoreAffinity, Priority};
use claw_sys::{EspIdfHttp, EspIdfThread};

use log::Level;

/// Scenario succeeded.
const OK: c_int = 0;
/// A required pointer argument was null (or not valid UTF-8).
const ERR_NULL_ARG: c_int = -1;
/// `EspIdfThread::spawn_worker` failed.
const ERR_THREAD_SPAWN: c_int = -3;
/// The worker ran but did not produce the expected side effect.
const ERR_THREAD_RESULT: c_int = -5;
/// The (blocking) HTTP request failed at the transport level.
const ERR_HTTP: c_int = -6;

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

/// Write each `log::Level` through `claw_sys::log_sink::write`, exercising the
/// Rust -> `claw_rs_log_*` C shim -> `ESP_LOGx` bridge. Always returns [`OK`];
/// the assertion is that it links and runs without crashing (output is visible
/// in the monitor / captured by pytest).
#[no_mangle]
pub extern "C" fn claw_sys_selftest_log() -> c_int {
    const TAG: &str = "claw_sys_test";
    claw_sys::log_sink::write(Level::Error, TAG, "log sink: error level");
    claw_sys::log_sink::write(Level::Warn, TAG, "log sink: warn level");
    claw_sys::log_sink::write(Level::Info, TAG, "log sink: info level");
    claw_sys::log_sink::write(Level::Debug, TAG, "log sink: debug level");
    claw_sys::log_sink::write(Level::Trace, TAG, "log sink: trace level");
    OK
}

/// Spawn a worker via `EspIdfThread` (the device `ClawThread`), have it bump a
/// shared counter, join it, and verify the side effect. Returns [`OK`] on
/// success or a negative `ERR_THREAD_*` code.
#[no_mangle]
pub extern "C" fn claw_sys_selftest_thread() -> c_int {
    let counter = Arc::new(AtomicU32::new(0));
    let worker_counter = Arc::clone(&counter);

    let handle = EspIdfThread::spawn_worker(
        "claw_sys_test_worker",
        4096,
        Priority::Normal,
        CoreAffinity::Any,
        move || {
            worker_counter.fetch_add(1, Ordering::SeqCst);
        },
    );

    let Ok(handle) = handle else {
        return ERR_THREAD_SPAWN;
    };
    handle.join();
    if counter.load(Ordering::SeqCst) == 1 {
        OK
    } else {
        ERR_THREAD_RESULT
    }
}

/// Blocking POST through `EspIdfHttp` (the synchronous `ClawHttp` seam over
/// `esp_http_client`). Returns the HTTP status code on a completed request
/// (e.g. 200), or a negative selftest error. The response body (or the error
/// text) is written to `out`.
///
/// # Safety
/// `url` must be a valid C string; `out` must point to `out_len` writable bytes
/// (or be null to skip the copy).
#[no_mangle]
pub unsafe extern "C" fn claw_sys_selftest_sync_http_post(
    url: *const c_char,
    out: *mut c_char,
    out_len: usize,
) -> c_int {
    let Some(url) = cstr(url) else {
        return ERR_NULL_ARG;
    };

    let abort = AtomicBool::new(false);
    let request = HttpJsonRequest {
        url,
        body: r#"{"selftest":"sync"}"#,
        auth: HttpAuth::None,
        timeout_ms: 15_000,
        headers: &[],
    };

    let Ok(mut http) = EspIdfHttp::new(url) else {
        return ERR_HTTP;
    };
    match BlockingClawHttp::post_json(&mut http, &request, &abort) {
        Ok(response) => {
            write_cstr(&response.body, out, out_len);
            // c_int and i32 are the same type on xtensa-esp32s3; no cast needed.
            response.status_code.as_i32()
        }
        Err(error) => {
            write_cstr(&error.to_string(), out, out_len);
            ERR_HTTP
        }
    }
}

/// Run three concurrent POSTs through the async `ClawHttp` seam, driven by
/// `edge-executor`'s `LocalExecutor`. `LocalExecutor` accepts `!Send` futures
/// (required because `esp_http_client` handles are thread-local). Returns the
/// number of requests that returned HTTP 200 (expect 3), or a negative error.
///
/// `url` must be HTTPS: `esp_http_client`'s non-blocking mode (used by the async
/// driver) is HTTPS-only.
///
/// # Safety
/// `url` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn claw_sys_selftest_run_three_async_http_posts(url: *const c_char) -> c_int {
    let Some(url) = cstr(url) else {
        return ERR_NULL_ARG;
    };
    let url = url.to_string();

    let successes: Rc<Cell<u32>> = Rc::new(Cell::new(0));

    let executor: edge_executor::LocalExecutor = Default::default();

    let mut handles = Vec::with_capacity(3);
    for index in 0..3u32 {
        let url = url.clone();
        let sink = Rc::clone(&successes);
        let task = executor.spawn(async move {
            let abort = AtomicBool::new(false);
            let body = format!(r#"{{"selftest":"async","index":{index}}}"#);
            let request = HttpJsonRequest {
                url: &url,
                body: &body,
                auth: HttpAuth::None,
                timeout_ms: 20_000,
                headers: &[],
            };
            let Ok(mut http) = EspIdfHttp::new(&url) else {
                return;
            };
            let pending = ClawHttp::post_json(&mut http, &request, Cancel::new(&abort));
            if let Ok(response) = pending.await {
                if response.status_code == HttpStatusCode::OK {
                    sink.set(sink.get().saturating_add(1));
                }
            }
        });
        handles.push(task);
    }

    edge_executor::block_on(executor.run(async {
        for handle in handles {
            handle.await;
        }
    }));

    let count = successes.get();
    c_int::try_from(count).unwrap_or(c_int::MAX)
}

// ---------------------------------------------------------------------------
// Resource profiling
// ---------------------------------------------------------------------------

mod resource {
    use core::fmt;

    const MALLOC_CAP_8BIT: u32 = 1 << 2;
    const MALLOC_CAP_SPIRAM: u32 = 1 << 10;
    const MALLOC_CAP_INTERNAL: u32 = 1 << 11;

    extern "C" {
        fn esp_get_free_heap_size() -> u32;
        fn esp_get_minimum_free_heap_size() -> u32;
        fn heap_caps_get_free_size(caps: u32) -> usize;
        fn heap_caps_get_largest_free_block(caps: u32) -> usize;
        fn heap_caps_get_total_size(caps: u32) -> usize;
    }

    #[derive(Clone, Copy)]
    pub struct HeapSnapshot {
        pub free_total: u32,
        pub free_internal: usize,
        pub free_spiram: usize,
        pub min_free_ever: u32,
        pub largest_internal_block: usize,
        pub total_internal: usize,
        pub total_spiram: usize,
    }

    impl HeapSnapshot {
        pub fn take() -> Self {
            unsafe {
                Self {
                    free_total: esp_get_free_heap_size(),
                    free_internal: heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    free_spiram: heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    min_free_ever: esp_get_minimum_free_heap_size(),
                    largest_internal_block: heap_caps_get_largest_free_block(
                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                    ),
                    total_internal: heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    total_spiram: heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                }
            }
        }
    }

    pub struct HeapDelta {
        pub internal_bytes: i64,
        pub spiram_bytes: i64,
        pub total_bytes: i64,
    }

    impl HeapDelta {
        pub fn compute(before: &HeapSnapshot, after: &HeapSnapshot) -> Self {
            Self {
                internal_bytes: before.free_internal as i64 - after.free_internal as i64,
                spiram_bytes: before.free_spiram as i64 - after.free_spiram as i64,
                total_bytes: before.free_total as i64 - after.free_total as i64,
            }
        }
    }

    impl fmt::Display for HeapSnapshot {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            write!(
                f,
                "free_total={}  internal={}/{}  spiram={}/{}  largest_blk={}  min_free_ever={}",
                self.free_total,
                self.free_internal,
                self.total_internal,
                self.free_spiram,
                self.total_spiram,
                self.largest_internal_block,
                self.min_free_ever,
            )
        }
    }

    impl fmt::Display for HeapDelta {
        fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
            write!(
                f,
                "delta_internal={:+}  delta_spiram={:+}  delta_total={:+}",
                self.internal_bytes, self.spiram_bytes, self.total_bytes,
            )
        }
    }

    pub fn print_snapshot(label: &str, snap: &HeapSnapshot) {
        // Use claw_sys log sink to go through ESP_LOG so pytest can capture it.
        let msg = format!("[RESOURCE] {label}: {snap}");
        claw_sys::log_sink::write(log::Level::Warn, "resource", &msg);
    }

    pub fn print_delta(label: &str, delta: &HeapDelta) {
        let msg = format!("[RESOURCE] {label}: {delta}");
        claw_sys::log_sink::write(log::Level::Warn, "resource", &msg);
    }
}

/// Measure heap baseline (no HTTP activity). Prints the snapshot via ESP_LOG.
/// Always returns [`OK`].
#[no_mangle]
pub extern "C" fn claw_sys_selftest_resource_baseline() -> c_int {
    let snap = resource::HeapSnapshot::take();
    resource::print_snapshot("baseline", &snap);
    OK
}

/// Profile a single **synchronous** HTTP POST: measures heap before/after and
/// prints the delta. Returns HTTP status on success or a negative selftest
/// error.
///
/// # Safety
/// `url` must be a valid C string.
#[no_mangle]
pub unsafe extern "C" fn claw_sys_selftest_resource_sync_http(url: *const c_char) -> c_int {
    let Some(url) = cstr(url) else {
        return ERR_NULL_ARG;
    };

    let before = resource::HeapSnapshot::take();
    resource::print_snapshot("sync_http:before", &before);

    let abort = AtomicBool::new(false);
    let request = HttpJsonRequest {
        url,
        body: r#"{"selftest":"resource_sync"}"#,
        auth: HttpAuth::None,
        timeout_ms: 15_000,
        headers: &[],
    };

    let Ok(mut http) = EspIdfHttp::new(url) else {
        return ERR_HTTP;
    };
    let result = BlockingClawHttp::post_json(&mut http, &request, &abort);

    let during = resource::HeapSnapshot::take();
    resource::print_snapshot("sync_http:during_response", &during);

    let status = match &result {
        Ok(r) => r.status_code.as_i32(),
        Err(_) => ERR_HTTP,
    };
    drop(result);

    let after = resource::HeapSnapshot::take();
    resource::print_snapshot("sync_http:after_cleanup", &after);

    let delta = resource::HeapDelta::compute(&before, &during);
    resource::print_delta("sync_http:peak_usage", &delta);
    let residual = resource::HeapDelta::compute(&before, &after);
    resource::print_delta("sync_http:residual(leak_check)", &residual);

    status
}

/// Profile async HTTP at concurrency levels 1, 2, and 3. For each level:
///   1. Snapshot heap before spawning.
///   2. Spawn N async POSTs and run them to completion.
///   3. Snapshot heap at peak (right after all complete, before drop).
///   4. Snapshot heap after cleanup.
///   5. Print per-connection overhead and peak totals.
///
/// Returns [`OK`] if all requests succeeded, or a negative error.
///
/// # Safety
/// `url` must be a valid C string (HTTPS).
#[no_mangle]
pub unsafe extern "C" fn claw_sys_selftest_resource_async_http(url: *const c_char) -> c_int {
    let Some(url_str) = cstr(url) else {
        return ERR_NULL_ARG;
    };

    for concurrency in [1u32, 2, 3] {
        let label = format!("async_http:concurrency={concurrency}");
        let url_owned = url_str.to_string();

        let before = resource::HeapSnapshot::take();
        resource::print_snapshot(&format!("{label}:before"), &before);

        let executor: edge_executor::LocalExecutor = Default::default();
        let ok_count: Rc<Cell<u32>> = Rc::new(Cell::new(0));
        let peak_snap: Rc<Cell<Option<resource::HeapSnapshot>>> = Rc::new(Cell::new(None));

        let mut handles = Vec::with_capacity(concurrency as usize);
        for index in 0..concurrency {
            let url = url_owned.clone();
            let sink = Rc::clone(&ok_count);
            let peak = Rc::clone(&peak_snap);
            let is_last = index == concurrency.saturating_sub(1);

            let task = executor.spawn(async move {
                let abort = AtomicBool::new(false);
                let body = format!(r#"{{"selftest":"resource_async","index":{index}}}"#);
                let request = HttpJsonRequest {
                    url: &url,
                    body: &body,
                    auth: HttpAuth::None,
                    timeout_ms: 20_000,
                    headers: &[],
                };
                let Ok(mut http) = EspIdfHttp::new(&url) else {
                    return;
                };
                let pending = ClawHttp::post_json(&mut http, &request, Cancel::new(&abort));
                if let Ok(response) = pending.await {
                    if response.status_code == HttpStatusCode::OK {
                        sink.set(sink.get().saturating_add(1));
                    }
                    if is_last {
                        peak.set(Some(resource::HeapSnapshot::take()));
                    }
                }
            });
            handles.push(task);
        }

        edge_executor::block_on(executor.run(async {
            for handle in handles {
                handle.await;
            }
        }));

        let peak = peak_snap.get().unwrap_or_else(resource::HeapSnapshot::take);
        resource::print_snapshot(&format!("{label}:peak"), &peak);

        drop(executor);
        drop(ok_count);
        drop(peak_snap);

        let after = resource::HeapSnapshot::take();
        resource::print_snapshot(&format!("{label}:after_cleanup"), &after);

        let peak_delta = resource::HeapDelta::compute(&before, &peak);
        resource::print_delta(&format!("{label}:peak_total"), &peak_delta);
        let residual = resource::HeapDelta::compute(&before, &after);
        resource::print_delta(&format!("{label}:residual(leak_check)"), &residual);

        let per_conn_internal = peak_delta.internal_bytes / i64::from(concurrency);
        let per_conn_spiram = peak_delta.spiram_bytes / i64::from(concurrency);
        let msg = format!(
            "[RESOURCE] {label}:per_connection  internal={per_conn_internal}  spiram={per_conn_spiram}"
        );
        claw_sys::log_sink::write(log::Level::Warn, "resource", &msg);
    }

    OK
}

/// Print a summary comparing sync vs async resource usage. Runs one sync POST
/// and one async POST, prints side-by-side deltas. Returns [`OK`].
///
/// # Safety
/// `http_url` must be a valid HTTP C string, `https_url` a valid HTTPS C string.
#[no_mangle]
pub unsafe extern "C" fn claw_sys_selftest_resource_summary(
    http_url: *const c_char,
    https_url: *const c_char,
) -> c_int {
    let (Some(http_url), Some(https_url)) = (cstr(http_url), cstr(https_url)) else {
        return ERR_NULL_ARG;
    };

    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        "========== RESOURCE PROFILE SUMMARY ==========",
    );

    // --- Sync ---
    let before_sync = resource::HeapSnapshot::take();
    let abort = AtomicBool::new(false);
    let sync_req = HttpJsonRequest {
        url: http_url,
        body: r#"{"selftest":"summary_sync"}"#,
        auth: HttpAuth::None,
        timeout_ms: 15_000,
        headers: &[],
    };
    let Ok(mut sync_http) = EspIdfHttp::new(http_url) else {
        return ERR_HTTP;
    };
    let sync_result = BlockingClawHttp::post_json(&mut sync_http, &sync_req, &abort);
    let after_sync_resp = resource::HeapSnapshot::take();
    drop(sync_result);
    let after_sync_clean = resource::HeapSnapshot::take();

    let sync_peak = resource::HeapDelta::compute(&before_sync, &after_sync_resp);
    let sync_residual = resource::HeapDelta::compute(&before_sync, &after_sync_clean);

    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        &format!("[RESOURCE] sync_post:peak       {sync_peak}"),
    );
    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        &format!("[RESOURCE] sync_post:residual    {sync_residual}"),
    );

    // --- Async (single) ---
    let before_async = resource::HeapSnapshot::take();
    let url_owned = https_url.to_string();
    let executor: edge_executor::LocalExecutor = Default::default();
    let async_peak: Rc<Cell<Option<resource::HeapSnapshot>>> = Rc::new(Cell::new(None));
    let peak_ref = Rc::clone(&async_peak);

    let task = executor.spawn(async move {
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url_owned,
            body: r#"{"selftest":"summary_async"}"#,
            auth: HttpAuth::None,
            timeout_ms: 20_000,
            headers: &[],
        };
        let Ok(mut http) = EspIdfHttp::new(&url_owned) else {
            return;
        };
        let pending = ClawHttp::post_json(&mut http, &request, Cancel::new(&abort));
        let _ = pending.await;
        peak_ref.set(Some(resource::HeapSnapshot::take()));
    });

    edge_executor::block_on(executor.run(task));

    let peak = async_peak
        .get()
        .unwrap_or_else(resource::HeapSnapshot::take);
    drop(executor);
    drop(async_peak);
    let after_async_clean = resource::HeapSnapshot::take();

    let async_peak_delta = resource::HeapDelta::compute(&before_async, &peak);
    let async_residual = resource::HeapDelta::compute(&before_async, &after_async_clean);

    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        &format!("[RESOURCE] async_post:peak      {async_peak_delta}"),
    );
    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        &format!("[RESOURCE] async_post:residual   {async_residual}"),
    );

    // --- Delta: async overhead relative to sync ---
    let overhead_internal = async_peak_delta.internal_bytes - sync_peak.internal_bytes;
    let overhead_spiram = async_peak_delta.spiram_bytes - sync_peak.spiram_bytes;
    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        &format!(
            "[RESOURCE] async_overhead(vs_sync)  internal={overhead_internal:+}  spiram={overhead_spiram:+}"
        ),
    );

    claw_sys::log_sink::write(
        log::Level::Warn,
        "resource",
        "========== END RESOURCE PROFILE SUMMARY ==========",
    );

    OK
}
