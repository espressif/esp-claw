//! `ClawHttp` driver over `esp_http_client`, porting `claw_llm_http_transport.c`.
//!
//! The pure-Rust helpers (auth header construction, error-body parsing) are
//! host-testable; only the `esp_http_client` plumbing is gated to the espidf
//! target.

/// Decide the auth header `(name, value)` for the given auth type + key.
///
/// Mirrors `build_auth_header_value` + `auth_header_name`: returns `None` when
/// the key is empty or the auth type is `"none"`.
#[cfg_attr(not(any(test, target_os = "espidf")), allow(dead_code))]
pub(crate) fn build_auth_header(
    auth_type: Option<&str>,
    api_key: Option<&str>,
) -> Option<(&'static str, String)> {
    let kind = auth_type.unwrap_or("bearer");
    let key = api_key.unwrap_or("");
    if key.is_empty() || kind == "none" {
        return None;
    }
    Some(match kind {
        "api-key" => ("X-API-Key", key.to_string()),
        _ => ("Authorization", format!("Bearer {key}")),
    })
}

/// Build the error message for a non-200 response, mirroring
/// `parse_error_message_body`: prefer `error.message`, then top-level
/// `message`, else a truncated body echo.
#[cfg_attr(not(any(test, target_os = "espidf")), allow(dead_code))]
pub(crate) fn parse_error_message_body(body: &str, status: i32) -> String {
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
pub use espidf_driver::EspIdfHttp;

#[cfg(target_os = "espidf")]
mod espidf_driver {
    use super::{build_auth_header, parse_error_message_body};
    use claw_interface::http::{
        ClawHttp, ClawHttpAsync, HttpError, HttpJsonRequest, HttpResponse, HttpResponseFuture,
    };
    use core::ffi::{c_char, c_int, c_void};
    use core::future::Future;
    use core::pin::Pin;
    use core::sync::atomic::{AtomicBool, Ordering};
    use core::task::{Context, Poll};
    use std::ffi::CString;

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
        fn esp_http_client_perform(client: *mut c_void) -> c_int;
        fn esp_http_client_get_status_code(client: *mut c_void) -> c_int;
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

    /// A persistent `esp_http_client` connection owned by [`EspIdfHttp`].
    ///
    /// Created lazily on the first blocking request and **reused** by every
    /// subsequent one (keep-alive is enabled at init), so the TCP connection and
    /// TLS handshake are paid once per endpoint instead of per request. The
    /// handle is torn down on `Drop` (RAII).
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
        /// Initialize a reusable keep-alive client. The per-request method,
        /// headers, and body are applied later by [`EspClient::execute`].
        fn new(request: &HttpJsonRequest) -> Result<EspClient, HttpError> {
            // `url` is parsed/copied by `esp_http_client_init`; it only needs to
            // stay alive until that call returns.
            let url = CString::new(request.url).map_err(|_| HttpError::InvalidUrl)?;
            let mut ctx = Box::new(RequestCtx {
                body: Vec::with_capacity(4096),
                abort: core::ptr::null(),
            });

            let mut config: esp_http_client_config_t = unsafe { core::mem::zeroed() };
            config.url = url.as_ptr();
            config.event_handler = Some(http_event_handler);
            config.user_data = (&mut *ctx as *mut RequestCtx) as *mut c_void;
            config.timeout_ms = request.timeout_ms as c_int;
            config.buffer_size = 4096;
            config.buffer_size_tx = 4096;
            config.crt_bundle_attach = Some(esp_crt_bundle_attach);
            // Reuse the underlying TCP/TLS connection across requests to the same
            // endpoint rather than reconnecting on every call.
            config.keep_alive_enable = true;

            let raw = unsafe { esp_http_client_init(&config) };
            if raw.is_null() {
                return Err(HttpError::ClientInitFailed);
            }
            Ok(EspClient { raw, ctx })
        }

        /// Apply this request's URL/method/headers/body to the persistent client
        /// and run one blocking transfer.
        ///
        /// Headers are re-set every call (same key overwrites); callers that send
        /// a stable header set per endpoint — like the LLM transport — get the
        /// expected wire format. A header present on an earlier request but absent
        /// here would persist, which the current callers never rely on.
        fn execute(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.ctx.body.clear();
            // Only dereferenced by the event handler during the blocking
            // `perform` below, where `abort` is guaranteed live.
            self.ctx.abort = abort as *const _;

            // `set_url` copies the string internally; `set_post_field` stores the
            // pointer (no copy), so `body` must outlive the blocking perform.
            let url = CString::new(request.url).map_err(|_| HttpError::InvalidUrl)?;
            let body = CString::new(request.body).map_err(|_| HttpError::InvalidBody)?;

            unsafe {
                esp_http_client_set_url(self.raw, url.as_ptr());
                esp_http_client_set_method(self.raw, HTTP_METHOD_POST);

                let ct_key = CString::new("Content-Type").unwrap();
                let ct_val = CString::new("application/json").unwrap();
                esp_http_client_set_header(self.raw, ct_key.as_ptr(), ct_val.as_ptr());

                if let Some((name, value)) = build_auth_header(request.auth_type, request.api_key) {
                    let k = CString::new(name).unwrap();
                    if let Ok(v) = CString::new(value) {
                        esp_http_client_set_header(self.raw, k.as_ptr(), v.as_ptr());
                    }
                }
                for h in request.headers {
                    if h.name.is_empty() {
                        continue;
                    }
                    if let (Ok(k), Ok(v)) = (CString::new(h.name), CString::new(h.value)) {
                        esp_http_client_set_header(self.raw, k.as_ptr(), v.as_ptr());
                    }
                }
                esp_http_client_set_post_field(
                    self.raw,
                    body.as_ptr(),
                    request.body.len() as c_int,
                );

                let err = esp_http_client_perform(self.raw);
                let aborted = abort.load(Ordering::Relaxed);
                if err != ESP_OK {
                    if aborted {
                        return Err(HttpError::Aborted);
                    }
                    return Err(HttpError::RequestFailed(err_name(err)));
                }
                if aborted {
                    return Err(HttpError::Aborted);
                }
                let status = esp_http_client_get_status_code(self.raw);
                let body_str = String::from_utf8_lossy(&self.ctx.body).into_owned();
                if status != 200 {
                    return Err(HttpError::UnexpectedStatus(parse_error_message_body(
                        &body_str, status,
                    )));
                }
                Ok(HttpResponse {
                    status_code: status,
                    body: body_str,
                })
            }
        }
    }

    /// `esp_http_client`-backed transport implementing both [`ClawHttp`]
    /// (blocking, cancelled via the in-band abort flag) and [`ClawHttpAsync`]
    /// (non-blocking `config.is_async` mode, cancelled by dropping the future).
    ///
    /// The blocking seam owns a persistent keep-alive [`EspClient`] (created on
    /// the first request and reused after) and is therefore driven through
    /// `&mut self`. The async seam uses an independent per-request client and
    /// does not touch the persistent connection.
    pub struct EspIdfHttp {
        // `None` until the first `post_json`; established lazily and reused.
        conn: Option<EspClient>,
    }

    impl EspIdfHttp {
        /// Create a transport with no open connection. The keep-alive client is
        /// established lazily on the first [`ClawHttp::post_json`].
        pub fn new() -> Self {
            Self { conn: None }
        }
    }

    impl Default for EspIdfHttp {
        fn default() -> Self {
            Self::new()
        }
    }

    // SAFETY: the only non-`Send`/`Sync` content is the raw `esp_http_client`
    // handle inside `conn`, reached exclusively through `&mut self`
    // (`post_json`) and so never used concurrently. Moving the transport between
    // threads (`Send`) is sound because the handle is touched only by its owner;
    // `&self` access (`ClawHttpAsync::transfer`) reads no shared state, so `Sync`
    // is sound too. The `ClawHttpAsync` supertrait requires both bounds.
    unsafe impl Send for EspIdfHttp {}
    unsafe impl Sync for EspIdfHttp {}

    impl ClawHttp for EspIdfHttp {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            if self.conn.is_none() {
                self.conn = Some(EspClient::new(request)?);
            }
            let client = self.conn.as_mut().ok_or(HttpError::ClientInitFailed)?;
            client.execute(request, abort)
        }
    }

    /// RAII owner of an in-flight async `esp_http_client` request.
    ///
    /// Holds everything that must outlive the repeated `esp_http_client_perform`
    /// calls and cleans the client up on drop, so cancelling the future (by
    /// dropping it) tears the request down without leaking the connection.
    struct AsyncRequest {
        client: *mut c_void,
        // Referenced by the client via `config.user_data`; the event handler
        // writes the response body here through that raw pointer, so the box
        // must stay alive and pinned in place for the whole request.
        ctx: Box<RequestCtx>,
        // `esp_http_client_set_post_field` stores the pointer (it does not copy
        // the body), so the buffer must outlive every perform call.
        _body: CString,
    }

    impl Drop for AsyncRequest {
        fn drop(&mut self) {
            unsafe { esp_http_client_cleanup(self.client) };
        }
    }

    impl AsyncRequest {
        /// Initialize a non-blocking client and apply method/headers/body once.
        /// After this returns the request is ready to be driven by repeated
        /// [`AsyncRequest::perform`] calls.
        fn new(request: &HttpJsonRequest) -> Result<AsyncRequest, HttpError> {
            // `url` only needs to stay alive until `esp_http_client_init`
            // returns (it parses and copies the URL internally).
            let url = CString::new(request.url).map_err(|_| HttpError::InvalidUrl)?;
            let body = CString::new(request.body).map_err(|_| HttpError::InvalidBody)?;
            // The async seam cancels by dropping the future (see `transfer`),
            // which runs `Drop` and tears the client down — so no abort flag is
            // wired into the event handler here.
            let mut ctx = Box::new(RequestCtx {
                body: Vec::with_capacity(4096),
                abort: core::ptr::null(),
            });

            let mut config: esp_http_client_config_t = unsafe { core::mem::zeroed() };
            config.url = url.as_ptr();
            config.event_handler = Some(http_event_handler);
            config.user_data = (&mut *ctx as *mut RequestCtx) as *mut c_void;
            config.timeout_ms = request.timeout_ms as c_int;
            config.buffer_size = 4096;
            config.buffer_size_tx = 4096;
            config.crt_bundle_attach = Some(esp_crt_bundle_attach);
            // Non-blocking mode: perform returns ESP_ERR_HTTP_EAGAIN while the
            // transfer is in progress. Only supported over HTTPS.
            config.is_async = true;

            let client = unsafe { esp_http_client_init(&config) };
            if client.is_null() {
                return Err(HttpError::ClientInitFailed);
            }
            // Own the client immediately so any early return below still cleans
            // it up via `Drop`.
            let request_state = AsyncRequest {
                client,
                ctx,
                _body: body,
            };

            unsafe {
                esp_http_client_set_method(client, HTTP_METHOD_POST);
                let ct_key = CString::new("Content-Type").unwrap();
                let ct_val = CString::new("application/json").unwrap();
                esp_http_client_set_header(client, ct_key.as_ptr(), ct_val.as_ptr());

                if let Some((name, value)) = build_auth_header(request.auth_type, request.api_key) {
                    let k = CString::new(name).unwrap();
                    if let Ok(v) = CString::new(value) {
                        esp_http_client_set_header(client, k.as_ptr(), v.as_ptr());
                    }
                }
                for h in request.headers {
                    if h.name.is_empty() {
                        continue;
                    }
                    if let (Ok(k), Ok(v)) = (CString::new(h.name), CString::new(h.value)) {
                        esp_http_client_set_header(client, k.as_ptr(), v.as_ptr());
                    }
                }
                esp_http_client_set_post_field(
                    client,
                    request_state._body.as_ptr(),
                    request.body.len() as c_int,
                );
            }
            Ok(request_state)
        }

        /// Run one non-blocking transfer step. `Ok(None)` means the transfer is
        /// still in progress (caller should yield and poll again); `Ok(Some(_))`
        /// is the finished response.
        fn perform(&self) -> Result<Option<HttpResponse>, HttpError> {
            let err = unsafe { esp_http_client_perform(self.client) };
            if err == ESP_ERR_HTTP_EAGAIN {
                return Ok(None);
            }
            if err != ESP_OK {
                return Err(HttpError::RequestFailed(err_name(err)));
            }
            let status = unsafe { esp_http_client_get_status_code(self.client) };
            let body = String::from_utf8_lossy(&self.ctx.body).into_owned();
            if status != 200 {
                return Err(HttpError::UnexpectedStatus(parse_error_message_body(
                    &body, status,
                )));
            }
            Ok(Some(HttpResponse {
                status_code: status,
                body,
            }))
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
        fn transfer<'a>(&'a self, request: &'a HttpJsonRequest<'a>) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                let state = AsyncRequest::new(request)?;
                loop {
                    if let Some(response) = state.perform()? {
                        return Ok(response);
                    }
                    yield_once().await;
                }
            })
        }
    }
}
