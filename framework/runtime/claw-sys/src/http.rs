//! `ClawHttp` driver over `esp_http_client`, porting `claw_llm_http_transport.c`.
//!
//! The pure-Rust helpers (auth header construction, error-body parsing) are
//! host-testable; only the `esp_http_client` plumbing is gated to the espidf
//! target.

use claw_interface::http::{HttpAuth, HttpStatusCode};

/// Decide the auth header `(name, value)` for the given auth mode.
///
/// Mirrors `build_auth_header_value` + `auth_header_name`.
#[cfg_attr(not(any(test, target_os = "espidf")), allow(dead_code))]
pub(crate) fn build_auth_header(auth: HttpAuth<'_>) -> Option<(&'static str, String)> {
    auth.header()
}

/// Build the error message for a non-200 response, mirroring
/// `parse_error_message_body`: prefer `error.message`, then top-level
/// `message`, else a truncated body echo.
#[cfg_attr(not(any(test, target_os = "espidf")), allow(dead_code))]
pub(crate) fn parse_error_message_body(body: &str, status: HttpStatusCode) -> String {
    if body.is_empty() {
        return format!("HTTP {status}");
    }
    match serde_json::from_str::<serde_json::Value>(body)
        .ok()
        .and_then(|root| extract_message(&root))
    {
        Some(msg) => format!("HTTP {status}: {msg}"),
        None => format!("HTTP {status}: {}", truncate(body, 160)),
    }
}

/// First non-empty string among `error.message` then top-level `message`.
#[cfg_attr(not(any(test, target_os = "espidf")), allow(dead_code))]
fn extract_message(root: &serde_json::Value) -> Option<String> {
    let nested = root.get("error").and_then(|e| e.get("message"));
    [nested, root.get("message")]
        .into_iter()
        .flatten()
        .filter_map(serde_json::Value::as_str)
        .find(|s| !s.is_empty())
        .map(str::to_owned)
}

#[cfg_attr(not(any(test, target_os = "espidf")), allow(dead_code))]
pub(crate) fn truncate(s: &str, max: usize) -> &str {
    if s.len() <= max {
        return s;
    }
    let mut end = max;
    while !s.is_char_boundary(end) {
        end -= 1;
    }
    &s[..end]
}

#[cfg(target_os = "espidf")]
pub use espidf_driver::{EspIdfHttp, EspIdfHttpOneShot};

#[cfg(target_os = "espidf")]
mod espidf_driver {
    use super::{build_auth_header, parse_error_message_body};
    use claw_interface::http::{
        Cancel, ClawHttp, ClawHttpAsync, HttpError, HttpJsonRequest, HttpRequestFailure,
        HttpResponse, HttpResponseFuture, HttpStatusCode,
    };
    use core::ffi::{c_char, c_int, c_void};
    use core::future::Future;
    use core::pin::Pin;
    use core::sync::atomic::{AtomicBool, Ordering};
    use core::task::{Context, Poll};
    use std::ffi::CString;

    const DEFAULT_INITIAL_URL: &str = "http://127.0.0.1/";

    // `esp_err_t` sentinels from components/esp_common/include/esp_err.h.
    const ESP_OK: c_int = 0;
    const ESP_FAIL: c_int = -1;

    /// `esp_http_client_perform` return when the non-blocking request is still
    /// in progress (`ESP_ERR_HTTP_BASE + 7`, see `esp_http_client.h`).
    const ESP_ERR_HTTP_EAGAIN: c_int = 0x7007;

    // --- esp_http_client FFI ------------------------------------------------
    #[repr(C)]
    struct esp_http_client_event_t {
        event_id: c_int,
        client: *mut c_void,
        data: *mut c_void,
        data_len: c_int,
        user_data: *mut c_void,
        header_key: *mut c_char,
        header_value: *mut c_char,
    }

    // esp_http_client_event_id_t: ERROR=0, ON_CONNECTED=1, HEADERS_SENT=2,
    // ON_HEADER=3, ON_DATA=4, ON_FINISH=5, ...
    const HTTP_EVENT_ON_DATA: c_int = 4;
    const HTTP_METHOD_POST: c_int = 1;

    type HttpEventHandleCb = unsafe extern "C" fn(*mut esp_http_client_event_t) -> c_int;
    type CrtBundleAttachFn = unsafe extern "C" fn(*mut c_void) -> c_int;

    // Only the prefix fields we set are declared; the rest of the struct is
    // zeroed by the caller and ignored by us. esp_http_client reads the full
    // struct, so we must match its real layout. We therefore use the documented
    // field order from esp_http_client.h.
    #[repr(C)]
    struct esp_http_client_config_t {
        url: *const c_char,
        host: *const c_char,
        port: c_int,
        username: *const c_char,
        password: *const c_char,
        auth_type: c_int,
        path: *const c_char,
        query: *const c_char,
        cert_pem: *const c_char,
        cert_len: usize,
        client_cert_pem: *const c_char,
        client_cert_len: usize,
        client_key_pem: *const c_char,
        client_key_len: usize,
        client_key_password: *const c_char,
        client_key_password_len: usize,
        tls_version: c_int,
        user_agent: *const c_char,
        method: c_int,
        timeout_ms: c_int,
        disable_auto_redirect: bool,
        max_redirection_count: c_int,
        max_authorization_retries: c_int,
        event_handler: Option<HttpEventHandleCb>,
        transport_type: c_int,
        buffer_size: c_int,
        buffer_size_tx: c_int,
        user_data: *mut c_void,
        is_async: bool,
        use_global_ca_store: bool,
        skip_cert_common_name_check: bool,
        common_name: *const c_char,
        crt_bundle_attach: Option<CrtBundleAttachFn>,
        keep_alive_enable: bool,
        keep_alive_idle: c_int,
        keep_alive_interval: c_int,
        keep_alive_count: c_int,
        if_name: *mut c_void,
        // Remaining bitfields/ports/cfg are left out; esp_http_client only reads
        // them when the corresponding feature flags are set, none of which we
        // enable. The struct is zero-initialized, so trailing fields read as 0.
        _reserved: [u8; 64],
    }

    extern "C" {
        fn esp_http_client_init(config: *const esp_http_client_config_t) -> *mut c_void;
        fn esp_http_client_set_url(client: *mut c_void, url: *const c_char) -> c_int;
        fn esp_http_client_set_method(client: *mut c_void, method: c_int) -> c_int;
        fn esp_http_client_set_header(
            client: *mut c_void,
            key: *const c_char,
            value: *const c_char,
        ) -> c_int;
        fn esp_http_client_set_post_field(
            client: *mut c_void,
            data: *const c_char,
            len: c_int,
        ) -> c_int;
        fn esp_http_client_set_timeout_ms(client: *mut c_void, timeout_ms: c_int) -> c_int;
        fn esp_http_client_delete_all_headers(client: *mut c_void) -> c_int;
        fn esp_http_client_reset_redirect_counter(client: *mut c_void) -> c_int;
        fn esp_http_client_cancel_request(client: *mut c_void) -> c_int;
        fn esp_http_client_perform(client: *mut c_void) -> c_int;
        fn esp_http_client_get_status_code(client: *mut c_void) -> c_int;
        fn esp_http_client_close(client: *mut c_void) -> c_int;
        fn esp_http_client_cleanup(client: *mut c_void) -> c_int;
        fn esp_crt_bundle_attach(conf: *mut c_void) -> c_int;
        fn esp_err_to_name(err: c_int) -> *const c_char;
    }

    struct RequestCtx {
        body: Vec<u8>,
        abort: *const AtomicBool,
    }

    extern "C" fn http_event_handler(evt: *mut esp_http_client_event_t) -> c_int {
        unsafe {
            let evt = &*evt;
            let ctx = evt.user_data as *mut RequestCtx;
            if ctx.is_null() {
                return ESP_OK;
            }
            let ctx = &mut *ctx;
            if !ctx.abort.is_null() && (*ctx.abort).load(Ordering::Relaxed) {
                return ESP_FAIL;
            }
            if evt.event_id == HTTP_EVENT_ON_DATA && evt.data_len > 0 {
                let slice =
                    core::slice::from_raw_parts(evt.data as *const u8, evt.data_len as usize);
                ctx.body.extend_from_slice(slice);
            }
        }
        ESP_OK
    }

    fn err_name(err: c_int) -> String {
        unsafe {
            let p = esp_err_to_name(err);
            if p.is_null() {
                return format!("{err}");
            }
            std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned()
        }
    }

    fn check_client_call(err: c_int, operation: &'static str) -> Result<(), HttpError> {
        if err == ESP_OK {
            Ok(())
        } else {
            Err(HttpError::RequestFailed(HttpRequestFailure::driver(
                operation,
                err_name(err),
            )))
        }
    }

    fn header_cstring(value: &str, label: &'static str) -> Result<CString, HttpError> {
        CString::new(value).map_err(|_| {
            HttpError::RequestFailed(HttpRequestFailure::HeaderContainsNul { field: label })
        })
    }

    fn body_len_to_c_int(len: usize) -> Result<c_int, HttpError> {
        c_int::try_from(len)
            .map_err(|_| HttpError::RequestFailed(HttpRequestFailure::BodyTooLarge { len }))
    }

    fn timeout_to_c_int(timeout_ms: u32) -> Result<c_int, HttpError> {
        c_int::try_from(timeout_ms).map_err(|_| {
            HttpError::RequestFailed(HttpRequestFailure::TimeoutTooLarge { timeout_ms })
        })
    }

    fn status_code_from_c_int(status: c_int) -> Result<HttpStatusCode, HttpError> {
        let status = u16::try_from(status).map_err(|_| {
            HttpError::RequestFailed(HttpRequestFailure::InvalidStatusCode { status })
        })?;
        Ok(HttpStatusCode::new(status))
    }

    unsafe fn set_header(client: *mut c_void, name: &str, value: &str) -> Result<(), HttpError> {
        let key = header_cstring(name, "header name")?;
        let val = header_cstring(value, "header value")?;
        check_client_call(
            esp_http_client_set_header(client, key.as_ptr(), val.as_ptr()),
            "esp_http_client_set_header",
        )
    }

    fn cancel_raw_request(client: *mut c_void) {
        unsafe {
            let err = esp_http_client_cancel_request(client);
            if err != ESP_OK {
                let _ = esp_http_client_close(client);
            }
        }
    }

    fn close_raw_connection(client: *mut c_void) {
        unsafe {
            let _ = esp_http_client_close(client);
        }
    }

    enum ActiveRequestState {
        Prepared,
        InFlight,
        Finished,
    }

    struct ActiveRequestGuard {
        client: *mut c_void,
        state: ActiveRequestState,
    }

    impl ActiveRequestGuard {
        fn new(client: *mut c_void) -> Self {
            Self {
                client,
                state: ActiveRequestState::Prepared,
            }
        }

        fn mark_started(&mut self) {
            if matches!(self.state, ActiveRequestState::Prepared) {
                self.state = ActiveRequestState::InFlight;
            }
        }

        fn finish(&mut self) {
            self.state = ActiveRequestState::Finished;
        }

        fn cancel(&mut self) {
            if matches!(self.state, ActiveRequestState::InFlight) {
                cancel_raw_request(self.client);
            }
            self.finish();
        }
    }

    impl Drop for ActiveRequestGuard {
        fn drop(&mut self) {
            if matches!(self.state, ActiveRequestState::InFlight) {
                cancel_raw_request(self.client);
            }
        }
    }

    /// A persistent async-mode `esp_http_client` handle owned by [`EspIdfHttp`].
    ///
    /// Created when [`EspIdfHttp`] is constructed and reused by subsequent requests
    /// (`keep_alive_enable` + `is_async` are set at init). Each request updates
    /// URL, method, headers, timeout, and body before driving `perform`; the raw
    /// handle itself is torn down only on `Drop`.
    struct EspClient {
        raw: *mut c_void,
        // `config.user_data` points at this box for the client's whole life; the
        // event handler writes the response body here through that raw pointer,
        // so the box must outlive the client and must not move. `Box` keeps the
        // heap payload pinned even when the `EspClient` value itself is moved.
        ctx: Box<RequestCtx>,
    }

    impl Drop for EspClient {
        fn drop(&mut self) {
            unsafe { esp_http_client_cleanup(self.raw) };
        }
    }

    impl EspClient {
        /// Initialize a reusable async-mode keep-alive client. Per-request
        /// options are applied later by [`EspClient::prepare_request`].
        fn new(initial_url: &str) -> Result<EspClient, HttpError> {
            // `url` is parsed/copied by `esp_http_client_init`; it only needs to
            // stay alive until that call returns.
            let url = CString::new(initial_url).map_err(|_| HttpError::InvalidUrl)?;
            let mut ctx = Box::new(RequestCtx {
                body: Vec::with_capacity(4096),
                abort: core::ptr::null(),
            });

            let mut config: esp_http_client_config_t = unsafe { core::mem::zeroed() };
            config.url = url.as_ptr();
            config.event_handler = Some(http_event_handler);
            config.user_data = (&mut *ctx as *mut RequestCtx) as *mut c_void;
            config.buffer_size = 4096;
            config.buffer_size_tx = 4096;
            config.crt_bundle_attach = Some(esp_crt_bundle_attach);
            // Reuse the underlying TCP/TLS connection across requests when the
            // server allows it. `is_async` makes `perform` return EAGAIN between
            // non-blocking steps; the blocking compatibility path below simply
            // loops over those steps.
            config.keep_alive_enable = true;
            config.is_async = true;

            let raw = unsafe { esp_http_client_init(&config) };
            if raw.is_null() {
                return Err(HttpError::ClientInitFailed);
            }
            Ok(EspClient { raw, ctx })
        }

        /// Apply this request's URL/method/headers/body to the persistent client.
        ///
        /// The returned body buffer must stay alive until the request finishes
        /// because `esp_http_client_set_post_field` stores, rather than copies,
        /// its pointer.
        fn prepare_request(
            &mut self,
            request: &HttpJsonRequest,
            abort: *const AtomicBool,
        ) -> Result<CString, HttpError> {
            self.ctx.body.clear();
            self.ctx.abort = abort;

            // `set_url` copies the string internally; `set_post_field` stores the
            // pointer (no copy), so `body` must outlive the blocking perform.
            let url = CString::new(request.url).map_err(|_| HttpError::InvalidUrl)?;
            let body = CString::new(request.body).map_err(|_| HttpError::InvalidBody)?;
            let body_len = body_len_to_c_int(request.body.len())?;
            let timeout_ms = timeout_to_c_int(request.timeout_ms)?;

            unsafe {
                check_client_call(
                    esp_http_client_set_url(self.raw, url.as_ptr()),
                    "esp_http_client_set_url",
                )?;
                check_client_call(
                    esp_http_client_set_method(self.raw, HTTP_METHOD_POST),
                    "esp_http_client_set_method",
                )?;
                check_client_call(
                    esp_http_client_set_timeout_ms(self.raw, timeout_ms),
                    "esp_http_client_set_timeout_ms",
                )?;
                check_client_call(
                    esp_http_client_reset_redirect_counter(self.raw),
                    "esp_http_client_reset_redirect_counter",
                )?;
                check_client_call(
                    esp_http_client_delete_all_headers(self.raw),
                    "esp_http_client_delete_all_headers",
                )?;

                set_header(self.raw, "Content-Type", "application/json")?;

                if let Some((name, value)) = build_auth_header(request.auth) {
                    set_header(self.raw, name, &value)?;
                }
                for h in request.headers {
                    if h.name.is_empty() {
                        continue;
                    }
                    set_header(self.raw, h.name, h.value)?;
                }
                check_client_call(
                    esp_http_client_set_post_field(self.raw, body.as_ptr(), body_len),
                    "esp_http_client_set_post_field",
                )?;
            }
            Ok(body)
        }

        /// Run one non-blocking transfer step. `Ok(None)` means the transfer is
        /// still in progress (caller should yield and poll again); `Ok(Some(_))`
        /// is the finished response.
        fn perform_step(&self) -> Result<Option<HttpResponse>, HttpError> {
            let err = unsafe { esp_http_client_perform(self.raw) };
            if err == ESP_ERR_HTTP_EAGAIN {
                return Ok(None);
            }
            if err != ESP_OK {
                return Err(HttpError::RequestFailed(HttpRequestFailure::driver(
                    "esp_http_client_perform",
                    err_name(err),
                )));
            }
            let status =
                status_code_from_c_int(unsafe { esp_http_client_get_status_code(self.raw) })?;
            let body = String::from_utf8_lossy(&self.ctx.body).into_owned();
            if !status.is_success() {
                return Err(HttpError::UnexpectedStatus {
                    status,
                    message: parse_error_message_body(&body, status),
                });
            }
            Ok(Some(HttpResponse {
                status_code: status,
                body,
            }))
        }

        /// Cancel the active transfer without destroying the reusable client
        /// handle. Best-effort: cancellation itself reports [`HttpError::Aborted`]
        /// to the caller even if the ESP-IDF helper says there was no active
        /// socket yet.
        fn cancel_active_request(&self) {
            cancel_raw_request(self.raw);
        }

        /// Close the active socket after a transport-level failure while keeping
        /// the reusable client handle alive for the next request.
        fn close_failed_connection(&self, error: &HttpError) {
            if matches!(error, HttpError::RequestFailed(_)) {
                close_raw_connection(self.raw);
            }
        }

        /// Blocking compatibility path over the async-mode client. This keeps
        /// the single persistent handle model while the sync trait is still
        /// present during the migration.
        fn execute_blocking(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            let _body = self.prepare_request(request, abort as *const _)?;
            let mut started = false;
            loop {
                if abort.load(Ordering::Relaxed) {
                    if started {
                        self.cancel_active_request();
                    }
                    return Err(HttpError::Aborted);
                }
                match self.perform_step() {
                    Ok(Some(response)) => return Ok(response),
                    Ok(None) => {
                        started = true;
                        std::thread::yield_now();
                    }
                    Err(error) => {
                        if abort.load(Ordering::Relaxed) {
                            return Err(HttpError::Aborted);
                        }
                        self.close_failed_connection(&error);
                        return Err(error);
                    }
                }
            }
        }

        async fn execute_async(
            &mut self,
            request: &HttpJsonRequest<'_>,
            cancel: Cancel<'_>,
        ) -> Result<HttpResponse, HttpError> {
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            let _body = self.prepare_request(request, core::ptr::null())?;
            let mut active = ActiveRequestGuard::new(self.raw);
            loop {
                if cancel.is_cancelled() {
                    active.cancel();
                    return Err(HttpError::Aborted);
                }
                match self.perform_step() {
                    Ok(Some(response)) => {
                        active.finish();
                        return Ok(response);
                    }
                    Ok(None) => {
                        active.mark_started();
                        yield_once().await;
                    }
                    Err(error) => {
                        self.close_failed_connection(&error);
                        active.finish();
                        return Err(error);
                    }
                }
            }
        }
    }

    /// `esp_http_client`-backed transport implementing both [`ClawHttp`]
    /// (blocking, cancelled via the in-band abort flag) and [`ClawHttpAsync`]
    /// (non-blocking `config.is_async` mode).
    ///
    /// The transport owns one persistent keep-alive [`EspClient`] created at
    /// construction and reused until `EspIdfHttp` is dropped. Async cancellation
    /// cancels the active request/socket, not the client handle.
    pub struct EspIdfHttp {
        conn: EspClient,
    }

    impl EspIdfHttp {
        /// Create a transport with a configured reusable ESP-IDF client handle.
        ///
        /// ESP-IDF requires an initial URL (or host/path) at
        /// `esp_http_client_init` time. The URL is still overwritten from every
        /// [`HttpJsonRequest`] before `perform`, so this does not bind the
        /// transport to one endpoint.
        pub fn new(initial_url: &str) -> Result<Self, HttpError> {
            Ok(Self {
                conn: EspClient::new(initial_url)?,
            })
        }
    }

    impl Default for EspIdfHttp {
        fn default() -> Self {
            match Self::new(DEFAULT_INITIAL_URL) {
                Ok(http) => http,
                Err(_) => std::process::abort(),
            }
        }
    }

    impl ClawHttp for EspIdfHttp {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.conn.execute_blocking(request, abort)
        }
    }

    /// Stateless ESP-IDF HTTP transport that creates a fresh client handle for
    /// each request.
    ///
    /// Use this when the transport value itself must cross a Rust thread
    /// boundary before requests are executed. The concrete `esp_http_client`
    /// handle is still created and driven inside the task that performs the
    /// request.
    #[derive(Clone, Copy, Default)]
    pub struct EspIdfHttpOneShot;

    impl ClawHttp for EspIdfHttpOneShot {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            let mut http = EspIdfHttp::new(request.url)?;
            ClawHttp::post_json(&mut http, request, abort)
        }
    }

    /// Yields once to the executor, then resumes. Lets cooperatively-scheduled
    /// tasks run between `ESP_ERR_HTTP_EAGAIN` retries instead of spinning the
    /// CPU inside a single poll.
    async fn yield_once() {
        struct YieldOnce(bool);
        impl Future for YieldOnce {
            type Output = ();
            fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
                if self.0 {
                    Poll::Ready(())
                } else {
                    self.0 = true;
                    cx.waker().wake_by_ref();
                    Poll::Pending
                }
            }
        }
        YieldOnce(false).await
    }

    impl ClawHttpAsync for EspIdfHttp {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move { self.conn.execute_async(request, cancel).await })
        }
    }

    impl ClawHttpAsync for EspIdfHttpOneShot {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                let mut http = EspIdfHttp::new(request.url)?;
                ClawHttpAsync::post_json(&mut http, request, cancel).await
            })
        }
    }
}
