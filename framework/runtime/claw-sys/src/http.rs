//! `ClawHttp` driver over `esp_http_client`, porting `claw_llm_http_transport.c`.
//!
//! The pure-Rust helpers (auth header construction, error-body parsing) are
//! host-testable; only the `esp_http_client` plumbing is gated to the espidf
//! target.

use claw_interface::http::HttpStatusCode;

/// Build the error message for a non-200 response, mirroring
/// `parse_error_message_body`: prefer `error.message`, then top-level
/// `message`, else a truncated body echo.
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
fn parse_error_message_body(body: &str, status: HttpStatusCode) -> String {
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
#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
fn extract_message(root: &serde_json::Value) -> Option<String> {
    let nested = root.get("error").and_then(|e| e.get("message"));
    [nested, root.get("message")]
        .into_iter()
        .flatten()
        .filter_map(serde_json::Value::as_str)
        .find(|s| !s.is_empty())
        .map(str::to_owned)
}

#[cfg_attr(not(target_os = "espidf"), allow(dead_code))]
fn truncate(s: &str, max: usize) -> &str {
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
pub use espidf_driver::EspIdfHttp;

#[cfg(target_os = "espidf")]
mod espidf_driver {
    use super::parse_error_message_body;
    use claw_interface::http::{
        blocking, Cancel, ClawHttp, HttpError, HttpGetRequest, HttpJsonRequest, HttpRequestFailure,
        HttpResponse, HttpResponseFuture, HttpStatusCode,
    };
    use core::ffi::{c_char, c_int, c_void};
    use core::future::Future;
    use core::pin::Pin;
    use core::sync::atomic::{AtomicBool, Ordering};
    use core::task::{Context, Poll};
    use std::ffi::CString;
    use std::time::{Duration, Instant};

    const DEFAULT_INITIAL_URL: &str = "http://127.0.0.1/";

    // `esp_err_t` sentinels from components/esp_common/include/esp_err.h.
    const ESP_OK: c_int = 0;
    const ESP_FAIL: c_int = -1;

    /// `esp_http_client_perform` return when the non-blocking request is still
    /// in progress (`ESP_ERR_HTTP_BASE + 7`, see `esp_http_client.h`).
    const ESP_ERR_HTTP_EAGAIN: c_int = 0x7007;
    const ERRNO_NONE: c_int = 0;
    const ERRNO_EAGAIN: c_int = 11;
    const ERRNO_EINPROGRESS: c_int = 115;

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
    const HTTP_METHOD_GET: c_int = 0;
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
        fn esp_http_client_delete_header(client: *mut c_void, key: *const c_char) -> c_int;
        fn esp_http_client_reset_redirect_counter(client: *mut c_void) -> c_int;
        fn esp_http_client_cancel_request(client: *mut c_void) -> c_int;
        fn esp_http_client_perform(client: *mut c_void) -> c_int;
        fn esp_http_client_get_errno(client: *mut c_void) -> c_int;
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

    /// Wall-clock deadline for a whole `perform` loop.
    ///
    /// `esp_http_client_set_timeout_ms` only bounds a single non-blocking poll,
    /// not the overall request: after a mid-transfer transport failure (e.g. a
    /// peer connection reset) `esp_http_client_perform` can keep reporting
    /// `ESP_ERR_HTTP_EAGAIN` indefinitely, which would spin our step loop
    /// forever. This deadline gives the loop an overall budget so it aborts
    /// instead of hanging and flooding logs.
    struct Deadline {
        limit: Option<Instant>,
    }

    impl Deadline {
        fn new(timeout_ms: u32) -> Self {
            let limit = (timeout_ms > 0)
                .then(|| Instant::now().checked_add(Duration::from_millis(u64::from(timeout_ms))))
                .flatten();
            Self { limit }
        }

        fn expired(&self) -> bool {
            self.limit.is_some_and(|limit| Instant::now() >= limit)
        }
    }

    fn timeout_error(timeout_ms: u32) -> HttpError {
        HttpError::RequestFailed(HttpRequestFailure::driver(
            "esp_http_client_perform",
            format!("request timed out after {timeout_ms} ms"),
        ))
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
        // Names of the headers this code set on the reused client for the last
        // request. Before the next request we delete exactly these instead of
        // wiping every header, so the User-Agent/Host that
        // `esp_http_client_init` installed survive across requests (matching the
        // fresh-client C transport). Sending no User-Agent trips bot management
        // on some LLM API edges (e.g. DeepSeek behind TencentEdgeOne -> 418).
        applied_headers: Vec<CString>,
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
            Ok(EspClient {
                raw,
                ctx,
                applied_headers: Vec::new(),
            })
        }

        /// Set a request header on the reused client and remember its name so
        /// the next request can remove it. Leaves headers we never touch (the
        /// User-Agent/Host from `esp_http_client_init`) in place.
        fn apply_header(&mut self, name: &str, value: &str) -> Result<(), HttpError> {
            unsafe { set_header(self.raw, name, value)? };
            if let Ok(cname) = CString::new(name) {
                if !self
                    .applied_headers
                    .iter()
                    .any(|existing| existing.as_bytes() == cname.as_bytes())
                {
                    self.applied_headers.push(cname);
                }
            }
            Ok(())
        }

        /// Remove the headers set by the previous request. Best-effort: a header
        /// may already be absent. Unlike `esp_http_client_delete_all_headers`,
        /// this preserves the init-time User-Agent/Host.
        fn clear_applied_headers(&mut self) {
            for name in self.applied_headers.drain(..) {
                unsafe { esp_http_client_delete_header(self.raw, name.as_ptr()) };
            }
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
            self.prepare_base_request(
                request.url,
                request.timeout_ms,
                request.auth,
                request.headers,
                abort,
            )?;

            // `set_post_field` stores the pointer (no copy), so `body` must
            // outlive the blocking perform.
            let body = CString::new(request.body).map_err(|_| HttpError::InvalidBody)?;
            let body_len = body_len_to_c_int(request.body.len())?;

            unsafe {
                check_client_call(
                    esp_http_client_set_method(self.raw, HTTP_METHOD_POST),
                    "esp_http_client_set_method",
                )?;
            }
            // Auth and extra headers were already applied by
            // `prepare_base_request`; only the POST content type is added here.
            self.apply_header("Content-Type", "application/json")?;
            unsafe {
                check_client_call(
                    esp_http_client_set_post_field(self.raw, body.as_ptr(), body_len),
                    "esp_http_client_set_post_field",
                )?;
            }
            Ok(body)
        }

        /// Apply common request fields and clear the response accumulator.
        fn prepare_base_request(
            &mut self,
            url: &str,
            timeout_ms: u32,
            auth: claw_interface::HttpAuth<'_>,
            headers: &[claw_interface::HttpHeader<'_>],
            abort: *const AtomicBool,
        ) -> Result<(), HttpError> {
            self.ctx.body.clear();
            self.ctx.abort = abort;

            // `set_url` copies the string internally; it only needs to live for
            // the duration of the call.
            let url = CString::new(url).map_err(|_| HttpError::InvalidUrl)?;
            let timeout_ms = timeout_to_c_int(timeout_ms)?;

            unsafe {
                check_client_call(
                    esp_http_client_set_url(self.raw, url.as_ptr()),
                    "esp_http_client_set_url",
                )?;
                check_client_call(
                    esp_http_client_set_timeout_ms(self.raw, timeout_ms),
                    "esp_http_client_set_timeout_ms",
                )?;
                check_client_call(
                    esp_http_client_reset_redirect_counter(self.raw),
                    "esp_http_client_reset_redirect_counter",
                )?;
            }
            // Delete only the headers we added last time, preserving the
            // init-time User-Agent/Host, then apply this request's headers.
            self.clear_applied_headers();
            self.set_auth_and_extra_headers(auth, headers)
        }

        fn set_auth_and_extra_headers(
            &mut self,
            auth: claw_interface::HttpAuth<'_>,
            headers: &[claw_interface::HttpHeader<'_>],
        ) -> Result<(), HttpError> {
            if let Some((name, value)) = auth.header() {
                self.apply_header(name, &value)?;
            }
            for header in headers {
                if header.name.is_empty() {
                    continue;
                }
                self.apply_header(header.name, header.value)?;
            }
            Ok(())
        }

        fn prepare_get_request(&mut self, request: &HttpGetRequest<'_>) -> Result<(), HttpError> {
            self.prepare_base_request(
                request.url,
                request.timeout_ms,
                request.auth,
                request.headers,
                core::ptr::null(),
            )?;
            unsafe {
                check_client_call(
                    esp_http_client_set_method(self.raw, HTTP_METHOD_GET),
                    "esp_http_client_set_method",
                )
            }
        }

        /// Run one non-blocking transfer step. `Ok(None)` means the transfer is
        /// still in progress (caller should yield and poll again); `Ok(Some(_))`
        /// is the finished response. An EAGAIN with a non-pending errno means
        /// the underlying connection has failed.
        fn perform_step(&self) -> Result<Option<HttpResponse>, HttpError> {
            let err = unsafe { esp_http_client_perform(self.raw) };
            if err == ESP_ERR_HTTP_EAGAIN {
                let transport_errno = unsafe { esp_http_client_get_errno(self.raw) };
                if matches!(
                    transport_errno,
                    ERRNO_NONE | ERRNO_EAGAIN | ERRNO_EINPROGRESS
                ) {
                    return Ok(None);
                }
                return Err(HttpError::RequestFailed(HttpRequestFailure::driver(
                    "esp_http_client_perform",
                    format!("async transport errno={transport_errno}"),
                )));
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

        /// Replace a failed persistent client with a fresh handle. This keeps
        /// stale keep-alive recovery inside the HTTP transport.
        fn reconnect(&mut self, initial_url: &str) -> Result<(), HttpError> {
            let replacement = Self::new(initial_url)?;
            let failed = std::mem::replace(self, replacement);
            drop(failed);
            Ok(())
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
            let deadline = Deadline::new(request.timeout_ms);
            let mut started = false;
            loop {
                if abort.load(Ordering::Relaxed) {
                    if started {
                        self.cancel_active_request();
                    }
                    return Err(HttpError::Aborted);
                }
                if deadline.expired() {
                    self.cancel_active_request();
                    return Err(timeout_error(request.timeout_ms));
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
            let deadline = Deadline::new(request.timeout_ms);
            let mut reconnected = false;
            'connection: loop {
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                if deadline.expired() {
                    return Err(timeout_error(request.timeout_ms));
                }
                let body = self.prepare_request(request, core::ptr::null())?;
                let mut active = ActiveRequestGuard::new(self.raw);
                loop {
                    if cancel.is_cancelled() {
                        active.cancel();
                        return Err(HttpError::Aborted);
                    }
                    if deadline.expired() {
                        active.cancel();
                        return Err(timeout_error(request.timeout_ms));
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
                            if !reconnected && matches!(error, HttpError::RequestFailed(_)) {
                                active.finish();
                                drop(body);
                                drop(active);
                                self.reconnect(request.url)?;
                                reconnected = true;
                                continue 'connection;
                            }
                            self.close_failed_connection(&error);
                            active.finish();
                            return Err(error);
                        }
                    }
                }
            }
        }

        async fn execute_get_async(
            &mut self,
            request: &HttpGetRequest<'_>,
            cancel: Cancel<'_>,
        ) -> Result<HttpResponse, HttpError> {
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            let deadline = Deadline::new(request.timeout_ms);
            let mut reconnected = false;
            'connection: loop {
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                if deadline.expired() {
                    return Err(timeout_error(request.timeout_ms));
                }
                self.prepare_get_request(request)?;
                let mut active = ActiveRequestGuard::new(self.raw);
                loop {
                    if cancel.is_cancelled() {
                        active.cancel();
                        return Err(HttpError::Aborted);
                    }
                    if deadline.expired() {
                        active.cancel();
                        return Err(timeout_error(request.timeout_ms));
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
                            if !reconnected && matches!(error, HttpError::RequestFailed(_)) {
                                active.finish();
                                drop(active);
                                self.reconnect(request.url)?;
                                reconnected = true;
                                continue 'connection;
                            }
                            self.close_failed_connection(&error);
                            active.finish();
                            return Err(error);
                        }
                    }
                }
            }
        }
    }

    /// `esp_http_client`-backed transport implementing both [`blocking::ClawHttp`]
    /// (blocking, cancelled via the in-band abort flag) and [`ClawHttp`]
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

    impl blocking::ClawHttp for EspIdfHttp {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.conn.execute_blocking(request, abort)
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

    impl ClawHttp for EspIdfHttp {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move { self.conn.execute_async(request, cancel).await })
        }

        fn get_json<'a>(
            &'a mut self,
            request: &'a HttpGetRequest<'a>,
            cancel: Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move { self.conn.execute_get_async(request, cancel).await })
        }
    }
}
