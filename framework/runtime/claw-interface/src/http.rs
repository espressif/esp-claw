//! The `ClawHttp` networking injection trait.
//!
//! Replaces `claw_llm_http_transport.c`. The espidf wiring implements this over
//! `esp_http_client`; host tests provide canned responses.

use core::fmt;
use core::future::Future;
use core::pin::Pin;
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::Poll;

/// A single extra request header (`name: value`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HttpHeader<'a> {
    pub name: &'a str,
    pub value: &'a str,
}

/// Authentication to apply to an HTTP request.
///
/// This keeps the key and its wire format in one value, so callers cannot build
/// invalid combinations such as "auth disabled but key present".
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum HttpAuth<'a> {
    /// Send no authentication header.
    #[default]
    None,
    /// Send `Authorization: Bearer <key>`. Empty keys send no auth header.
    Bearer(&'a str),
    /// Send `X-API-Key: <key>`. Empty keys send no auth header.
    ApiKey(&'a str),
}

impl<'a> HttpAuth<'a> {
    /// Build the wire header for this auth mode, if any.
    pub fn header(self) -> Option<(&'static str, String)> {
        match self {
            Self::None => None,
            Self::Bearer("") | Self::ApiKey("") => None,
            Self::Bearer(key) => Some(("Authorization", format!("Bearer {key}"))),
            Self::ApiKey(key) => Some(("X-API-Key", key.to_string())),
        }
    }
}

/// Parameters for a JSON POST, mirroring `claw_llm_http_json_request_t`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HttpJsonRequest<'a> {
    pub url: &'a str,
    pub body: &'a str,
    pub auth: HttpAuth<'a>,
    pub timeout_ms: u32,
    pub headers: &'a [HttpHeader<'a>],
}

/// Parameters for a JSON GET request.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HttpGetRequest<'a> {
    pub url: &'a str,
    pub auth: HttpAuth<'a>,
    pub timeout_ms: u32,
    pub headers: &'a [HttpHeader<'a>],
}

/// HTTP status code.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct HttpStatusCode(u16);

impl HttpStatusCode {
    pub const OK: Self = Self(200);
    pub const NO_CONTENT: Self = Self(204);

    pub const fn new(code: u16) -> Self {
        Self(code)
    }

    pub const fn as_u16(self) -> u16 {
        self.0
    }

    pub fn as_i32(self) -> i32 {
        i32::from(self.0)
    }

    pub const fn is_success(self) -> bool {
        self.0 >= 200 && self.0 < 300
    }
}

impl fmt::Display for HttpStatusCode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

impl From<HttpStatusCode> for u16 {
    fn from(status: HttpStatusCode) -> Self {
        status.as_u16()
    }
}

impl From<HttpStatusCode> for i32 {
    fn from(status: HttpStatusCode) -> Self {
        status.as_i32()
    }
}

/// A successful HTTP response, mirroring `claw_llm_http_response_t`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HttpResponse {
    pub status_code: HttpStatusCode,
    pub body: String,
}

/// Structured reason for a transport-level request failure.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum HttpRequestFailure {
    #[error("transport failed: {0}")]
    Transport(String),
    #[error("{operation} failed: {message}")]
    Driver {
        operation: &'static str,
        message: String,
    },
    #[error("{field} contains NUL byte")]
    HeaderContainsNul { field: &'static str },
    #[error("request body is too large: {len} bytes")]
    BodyTooLarge { len: usize },
    #[error("request timeout is too large: {timeout_ms} ms")]
    TimeoutTooLarge { timeout_ms: u32 },
    #[error("HTTP status code is outside u16 range: {status}")]
    InvalidStatusCode { status: i32 },
}

impl HttpRequestFailure {
    pub fn transport(message: impl Into<String>) -> Self {
        Self::Transport(message.into())
    }

    pub fn driver(operation: &'static str, message: impl Into<String>) -> Self {
        Self::Driver {
            operation,
            message: message.into(),
        }
    }
}

/// Rust-native HTTP transport failure.
///
/// `esp_err_t` mapping for the C ABI lives in `claw_capi::errmap::http_esp_err`.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum HttpError {
    #[error("HTTP request aborted by caller")]
    Aborted,
    #[error("invalid URL")]
    InvalidUrl,
    #[error("request body contains NUL byte")]
    InvalidBody,
    #[error("failed to create HTTP client")]
    ClientInitFailed,
    #[error("HTTP request failed: {0}")]
    RequestFailed(HttpRequestFailure),
    /// Non-success response; message matches C `parse_error_message_body` shape.
    #[error("{message}")]
    UnexpectedStatus {
        status: HttpStatusCode,
        message: String,
    },
}

pub mod blocking {
    //! Blocking HTTP transport seam.
    //!
    //! This namespace holds the synchronous compatibility surface. Agent runtime
    //! code should prefer the async [`crate::http::ClawHttp`].

    use super::{AtomicBool, HttpError, HttpJsonRequest, HttpResponse};
    #[cfg(feature = "realhttp")]
    use super::{HttpRequestFailure, HttpStatusCode};
    #[cfg(feature = "realhttp")]
    use core::sync::atomic::Ordering;
    #[cfg(feature = "realhttp")]
    use std::time::Duration;

    /// Networking injection point for blocking LLM clients.
    ///
    /// Thread-safety is **not** a property of this trait: it carries no
    /// `Send`/`Sync` supertrait bound. An implementation may hold a single,
    /// non-thread-safe connection handle and mutate it through `&mut self`.
    /// Concurrency is the *caller's* decision.
    pub trait ClawHttp {
        /// Equivalent to `claw_llm_http_post_json`. Returns the body on HTTP 2xx,
        /// otherwise an [`HttpError`]. `abort` is polled cooperatively; when set the
        /// request is cancelled (mirrors the C `volatile bool *abort_flag`).
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError>;
    }

    /// Host [`ClawHttp`] backed by a blocking `reqwest::blocking::Client`.
    ///
    /// This is the synchronous compatibility backend for host CLIs and live
    /// tests that intentionally run blocking HTTP. Async code should prefer the
    /// top-level [`crate::http::RealHttp`].
    #[cfg(feature = "realhttp")]
    #[derive(Debug, Clone, Default)]
    pub struct RealHttp {
        user_agent: Option<String>,
    }

    #[cfg(feature = "realhttp")]
    impl RealHttp {
        /// A client with no extra `User-Agent`.
        pub fn new() -> Self {
            Self::default()
        }

        /// A client that sends the given `User-Agent` on every request.
        pub fn with_user_agent(user_agent: impl Into<String>) -> Self {
            Self {
                user_agent: Some(user_agent.into()),
            }
        }
    }

    #[cfg(feature = "realhttp")]
    impl ClawHttp for RealHttp {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            if abort.load(Ordering::Acquire) {
                return Err(HttpError::Aborted);
            }

            let client = reqwest::blocking::Client::builder()
                .timeout(Duration::from_millis(u64::from(request.timeout_ms)))
                .build()
                .map_err(|_| HttpError::ClientInitFailed)?;

            let mut builder = client
                .post(request.url)
                .header("Content-Type", "application/json")
                .body(request.body.to_string());

            if let Some(user_agent) = &self.user_agent {
                builder = builder.header("User-Agent", user_agent);
            }

            if let Some((name, value)) = request.auth.header() {
                builder = builder.header(name, value);
            }
            for header in request.headers {
                builder = builder.header(header.name, header.value);
            }

            let response = builder.send().map_err(|error| {
                HttpError::RequestFailed(HttpRequestFailure::transport(error.to_string()))
            })?;
            let status_code = HttpStatusCode::new(response.status().as_u16());
            let body = response.text().map_err(|error| {
                HttpError::RequestFailed(HttpRequestFailure::transport(error.to_string()))
            })?;

            if !status_code.is_success() {
                return Err(HttpError::UnexpectedStatus {
                    status: status_code,
                    message: format!("HTTP {status_code}: {body}"),
                });
            }
            Ok(HttpResponse { status_code, body })
        }
    }
}

/// Boxed future returned by [`ClawHttp::post_json`].
///
/// Boxed (instead of an `async fn` in the trait) so [`ClawHttp`] stays object
/// safe. The future borrows `self` mutably, the request, and the cancellation
/// token, so it cannot outlive any of them.
pub type HttpResponseFuture<'a> =
    Pin<Box<dyn Future<Output = Result<HttpResponse, HttpError>> + 'a>>;

/// Cooperative cancellation token for [`ClawHttp`].
///
/// A thin, `Copy` wrapper over a caller-owned abort flag. Implementations check
/// the token cooperatively: a non-blocking driver should check it before each
/// transfer step and cancel its in-flight request when cancellation is observed.
/// Set the flag from any context (another task, an interrupt) to request
/// cancellation.
///
/// `Cancel` exists so the async seam never exposes a bare `&AtomicBool`; it is
/// the home for the abort signal and any future awaitable extension.
///
/// # Examples
///
/// ```
/// use claw_interface::Cancel;
/// use core::sync::atomic::{AtomicBool, Ordering};
///
/// let flag = AtomicBool::new(false);
/// let cancel = Cancel::new(&flag);
/// assert!(!cancel.is_cancelled());
///
/// flag.store(true, Ordering::Relaxed); // request cancellation from any context
/// assert!(cancel.is_cancelled());
///
/// // A token that never cancels, for callers that do not cancel:
/// assert!(!Cancel::never().is_cancelled());
/// ```
#[derive(Debug, Clone, Copy)]
pub struct Cancel<'a>(&'a AtomicBool);

impl<'a> Cancel<'a> {
    /// Wrap a caller-owned abort flag.
    pub fn new(flag: &'a AtomicBool) -> Self {
        Self(flag)
    }

    /// Whether cancellation has been requested.
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }
}

impl Cancel<'static> {
    /// A token that never cancels — for callers/tests that do not cancel.
    pub fn never() -> Self {
        static NEVER: AtomicBool = AtomicBool::new(false);
        Cancel(&NEVER)
    }
}

/// HTTP transport driven by a cooperative executor instead of blocking the
/// calling task for the whole request.
///
/// On ESP-IDF this maps onto `esp_http_client`'s non-blocking mode
/// (`config.is_async = true`): each poll runs one `esp_http_client_perform`
/// step and yields (`Poll::Pending`) while the call reports
/// `ESP_ERR_HTTP_EAGAIN`, letting other tasks run between steps.
///
/// Thread-safety is intentionally not a property of this base trait. Host
/// transports such as `reqwest::Client` may be `Send + Sync`, while embedded
/// transports may be task-local because they advance raw driver handles across
/// polls. Require `Send`/`Sync` at the caller boundary that actually crosses
/// threads.
pub trait ClawHttp {
    /// Async JSON POST.
    ///
    /// Implementations must observe `cancel` cooperatively and return
    /// [`HttpError::Aborted`] when cancellation wins. A non-blocking driver
    /// should release any in-flight request state when the returned future is
    /// dropped.
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a>;

    /// Async JSON GET for read-only capability clients.
    ///
    /// Implementations that only support POST keep the default rejection.
    /// Callers should surface it as a capability/tool invocation error rather
    /// than silently changing methods.
    fn get_json<'a>(
        &'a mut self,
        _request: &'a HttpGetRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async {
            Err(HttpError::RequestFailed(HttpRequestFailure::driver(
                "http get",
                "transport does not support async HTTP GET",
            )))
        })
    }
}

/// Drive `future` while checking `cancel` before each poll.
///
/// This is a helper for implementations that already have a cancellable
/// in-flight future. The cancellation side is poll-bound: setting the flag does
/// not wake a parked future by itself.
#[cfg_attr(not(feature = "realhttp"), allow(dead_code))]
fn cancel_on_poll<'a>(
    mut transfer: HttpResponseFuture<'a>,
    cancel: Cancel<'a>,
) -> HttpResponseFuture<'a> {
    Box::pin(async move {
        core::future::poll_fn(move |context| {
            if cancel.is_cancelled() {
                return Poll::Ready(Err(HttpError::Aborted));
            }
            transfer.as_mut().poll(context)
        })
        .await
    })
}

// ===========================================================================
// Reference implementations (feature-gated)
// ===========================================================================
//
// Host-only `ClawHttp` backends kept beside the trait so the duplicated doubles
// live in one place. They are NOT part of the platform-free seam, so each is
// opt-in and must never be enabled in a device build:
//
// - `httpmock`: scripted / failing / never-called test doubles.
// - `realhttp`: a blocking `reqwest` client for the host CLIs and live tests.

#[cfg(feature = "httpmock")]
mod httpmock {
    use core::future::Future;
    use core::pin::Pin;
    use core::task::{Context, Poll};
    use std::collections::VecDeque;
    use std::sync::atomic::AtomicBool;
    use std::sync::{Arc, Mutex, MutexGuard};

    use super::{
        blocking, ClawHttp, HttpError, HttpJsonRequest, HttpRequestFailure, HttpResponse,
        HttpResponseFuture, HttpStatusCode,
    };

    /// One scripted LLM round: a 200 body, or a transport-level error message.
    pub type ScriptStep = Result<String, String>;

    fn guard<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
        mutex
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
    }

    fn respond(step: ScriptStep) -> Result<HttpResponse, HttpError> {
        match step {
            Ok(body) => Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body,
            }),
            Err(message) => Err(HttpError::RequestFailed(HttpRequestFailure::transport(
                message,
            ))),
        }
    }

    fn into_steps(bodies: impl IntoIterator<Item = impl Into<String>>) -> VecDeque<ScriptStep> {
        bodies.into_iter().map(|body| Ok(body.into())).collect()
    }

    /// Replays scripted rounds in order; panics if the LLM is called more times
    /// than scripted, so an unexpected round fails loudly instead of silently.
    pub struct ScriptedHttp {
        steps: Mutex<VecDeque<ScriptStep>>,
    }

    impl ScriptedHttp {
        /// Script of all-success bodies served in order.
        pub fn new(bodies: impl IntoIterator<Item = impl Into<String>>) -> Self {
            Self {
                steps: Mutex::new(into_steps(bodies)),
            }
        }

        /// Script of mixed success / transport-error rounds served in order.
        pub fn with_steps(steps: impl IntoIterator<Item = ScriptStep>) -> Self {
            Self {
                steps: Mutex::new(steps.into_iter().collect()),
            }
        }

        /// Serve the next scripted step. Takes `&self` (state is interior-mutable)
        /// so the double works both owned and behind an `Arc`.
        fn serve(&self) -> Result<HttpResponse, HttpError> {
            let step = guard(&self.steps)
                .pop_front()
                .expect("ScriptedHttp: LLM called more times than scripted");
            respond(step)
        }
    }

    impl blocking::ClawHttp for ScriptedHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.serve()
        }
    }

    /// Share one script across clients that each *own* their transport (e.g. a
    /// `ClawApi<Arc<ScriptedHttp>>`): every clone replays from the same queue.
    impl blocking::ClawHttp for Arc<ScriptedHttp> {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.serve()
        }
    }

    /// Like [`ScriptedHttp`] but records every request body so a test can assert
    /// on the context the agent sent to the model.
    pub struct CapturingHttp {
        steps: Mutex<VecDeque<ScriptStep>>,
        captured: Mutex<Vec<String>>,
    }

    impl CapturingHttp {
        /// Script of all-success bodies; returns an `Arc` for sharing with the LLM.
        pub fn new(bodies: impl IntoIterator<Item = impl Into<String>>) -> Arc<Self> {
            Arc::new(Self {
                steps: Mutex::new(into_steps(bodies)),
                captured: Mutex::new(Vec::new()),
            })
        }

        /// Script of mixed success / transport-error rounds.
        pub fn with_steps(steps: impl IntoIterator<Item = ScriptStep>) -> Arc<Self> {
            Arc::new(Self {
                steps: Mutex::new(steps.into_iter().collect()),
                captured: Mutex::new(Vec::new()),
            })
        }

        /// Raw request bodies the agent sent, in call order.
        pub fn captured(&self) -> Vec<String> {
            guard(&self.captured).clone()
        }

        /// Request bodies parsed as JSON, in call order (unparseable → `Null`).
        pub fn captured_bodies(&self) -> Vec<serde_json::Value> {
            guard(&self.captured)
                .iter()
                .map(|body| match serde_json::from_str(body) {
                    Ok(value) => value,
                    Err(_) => serde_json::Value::Null,
                })
                .collect()
        }

        /// Number of LLM calls made so far.
        pub fn call_count(&self) -> usize {
            guard(&self.captured).len()
        }

        /// Record `request` and serve the next scripted step. Takes `&self` (state
        /// is interior-mutable) so the double works both owned and behind an `Arc`
        /// — the latter lets a test keep a handle to assert on `captured` after the
        /// client has consumed it.
        fn serve(&self, request: &HttpJsonRequest) -> Result<HttpResponse, HttpError> {
            guard(&self.captured).push(request.body.to_string());
            let step = guard(&self.steps)
                .pop_front()
                .expect("CapturingHttp: LLM called more times than scripted");
            respond(step)
        }
    }

    impl blocking::ClawHttp for CapturingHttp {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.serve(request)
        }
    }

    /// Lets a test hold an `Arc<CapturingHttp>` for inspection while a
    /// `ClawApi<Arc<CapturingHttp>>` owns a clone and drives it.
    impl blocking::ClawHttp for Arc<CapturingHttp> {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.serve(request)
        }
    }

    /// The script every [`SharedScriptHttp::default`] in the process shares.
    /// Installed by [`SharedScriptHttp::install`]; read at each construction.
    ///
    /// Process-global (not thread-local) so a `SharedScriptHttp` built on an
    /// orchestrator's drive **worker thread** replays the script a test installed
    /// on its own thread. Tests that install scripts must serialize with
    /// [`SharedScriptHttp::serialize`] so parallel tests do not clobber it.
    static SHARED_SCRIPT: Mutex<Option<Arc<Mutex<VecDeque<ScriptStep>>>>> = Mutex::new(None);
    /// Held by a test (via [`SharedScriptHttp::serialize`]) for the whole span in
    /// which it owns the process-global script.
    static SHARED_SCRIPT_LOCK: Mutex<()> = Mutex::new(());

    fn shared_script_slot(
    ) -> std::sync::MutexGuard<'static, Option<Arc<Mutex<VecDeque<ScriptStep>>>>> {
        SHARED_SCRIPT
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    /// A [`Default`]-constructible scripted transport for systems that mint their
    /// own clients and choose the transport by *type* (e.g. `FsAgentFactory<F, H>`
    /// / `AgentSystem` with `H = SharedScriptHttp`).
    ///
    /// A plain [`ScriptedHttp`] can't be injected into those, because the system
    /// constructs each `H::default()` internally. `SharedScriptHttp` bridges the
    /// gap: a test calls [`install`](Self::install) once, then **every**
    /// `SharedScriptHttp::default()` in the process shares the one script and pops
    /// from it in call order — reproducing the single-shared-script behavior of an
    /// injected [`ScriptedHttp`], including from a drive worker thread. Strict:
    /// panics if called more times than scripted, or if no script was installed.
    #[derive(Clone)]
    pub struct SharedScriptHttp {
        steps: Option<Arc<Mutex<VecDeque<ScriptStep>>>>,
    }

    impl SharedScriptHttp {
        /// Install the script shared by every later `SharedScriptHttp::default()`
        /// in the process. Call once before building the system under test, while
        /// holding the [`serialize`](Self::serialize) guard.
        pub fn install(bodies: impl IntoIterator<Item = impl Into<String>>) {
            let steps = Arc::new(Mutex::new(into_steps(bodies)));
            *shared_script_slot() = Some(steps);
        }

        /// Drop the installed script (so a later `default()` with no script fails
        /// loudly rather than replaying a stale one).
        pub fn clear() {
            *shared_script_slot() = None;
        }

        /// Serialize access to the process-global script. Hold the returned guard
        /// for the whole test body (install → submit → drain) so parallel tests
        /// never share one another's script.
        pub fn serialize() -> std::sync::MutexGuard<'static, ()> {
            SHARED_SCRIPT_LOCK
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
        }
    }

    impl Default for SharedScriptHttp {
        fn default() -> Self {
            Self {
                steps: shared_script_slot().clone(),
            }
        }
    }

    impl blocking::ClawHttp for SharedScriptHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            let steps = self.steps.as_ref().expect(
                "SharedScriptHttp: no script installed; call SharedScriptHttp::install(..)",
            );
            let step = guard(steps)
                .pop_front()
                .expect("SharedScriptHttp: LLM called more times than scripted");
            respond(step)
        }
    }

    /// Always fails the LLM round with a transport error.
    pub struct FailingHttp;

    impl blocking::ClawHttp for FailingHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            Err(HttpError::RequestFailed(HttpRequestFailure::transport(
                "simulated failure",
            )))
        }
    }

    /// Returns a no-op transport error; for wiring where the LLM is never driven.
    pub struct NoopHttp;

    impl blocking::ClawHttp for NoopHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            Err(HttpError::RequestFailed(HttpRequestFailure::transport(
                "noop",
            )))
        }
    }

    /// Panics if called — for tests that must never reach the LLM.
    pub struct NeverHttp;

    impl blocking::ClawHttp for NeverHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            panic!("NeverHttp: the LLM must not be called in this test");
        }
    }

    // -----------------------------------------------------------------------
    // Async-seam adapters: turn a blocking HTTP transport into async HTTP.
    // Host-only, so they live beside the blocking doubles instead of in the
    // platform-free seam, and never enter the device/default build.
    // -----------------------------------------------------------------------

    /// Adapts any blocking [`blocking::ClawHttp`] into an async HTTP transport by running the
    /// request to completion in a single poll.
    ///
    /// Intended for the host (CLIs, tests) where the blocking transports — the
    /// scripted doubles above, `RealHttp` — already exist and a real
    /// cooperative executor is unnecessary. It is **not** appropriate on-device:
    /// the wrapped call blocks the polling task for the whole request, defeating
    /// the purpose of the async seam (use the native `esp_http_client` driver
    /// there instead).
    pub struct BlockingHttpAdapter<T>(T);

    impl<T> BlockingHttpAdapter<T> {
        /// Wrap a blocking [`blocking::ClawHttp`] for the async seam.
        pub fn new(inner: T) -> Self {
            Self(inner)
        }
    }

    impl<T: Default> Default for BlockingHttpAdapter<T> {
        fn default() -> Self {
            Self(T::default())
        }
    }

    impl<T: blocking::ClawHttp> ClawHttp for BlockingHttpAdapter<T> {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: super::Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                // A blocking call cannot be interrupted mid-flight, so no abort
                // flag is threaded in. Once it returns Ok, cancellation belongs
                // to the caller's next checkpoint rather than being rewritten as
                // an HTTP abort.
                let never = AtomicBool::new(false);
                self.0.post_json(request, &never)
            })
        }
    }

    /// Yields to the executor exactly once, then resolves. Re-arms the waker
    /// before returning `Poll::Pending` so a cooperative executor re-polls
    /// instead of stalling — the same poll/wake handshake the on-device async
    /// driver performs between `esp_http_client` `ESP_ERR_HTTP_EAGAIN` steps.
    async fn yield_once() {
        struct YieldOnce(bool);
        impl Future for YieldOnce {
            type Output = ();
            fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<()> {
                if self.0 {
                    Poll::Ready(())
                } else {
                    self.0 = true;
                    context.waker().wake_by_ref();
                    Poll::Pending
                }
            }
        }
        YieldOnce(false).await
    }

    /// Adapts any blocking [`blocking::ClawHttp`] into an async HTTP transport that yields to
    /// the executor `yields` times **before** running the wrapped request to
    /// completion.
    ///
    /// This is a host/test simulation of a genuinely non-blocking transport: it
    /// reproduces the multi-poll, `Poll::Pending`-between-steps shape of the
    /// on-device `esp_http_client` driver (which yields on every
    /// `ESP_ERR_HTTP_EAGAIN`) without needing real hardware. Use it to drive a
    /// cooperative executor's poll/wake loop against the async HTTP seam:
    /// unlike [`BlockingHttpAdapter`] (which resolves in a single poll), its
    /// future returns `Poll::Pending` `yields` times first.
    ///
    /// Like [`BlockingHttpAdapter`], the final transfer step still calls the
    /// blocking [`blocking::ClawHttp`], so it is **not** appropriate on-device.
    pub struct YieldingHttpAdapter<T> {
        inner: T,
        yields: u32,
    }

    impl<T> YieldingHttpAdapter<T> {
        /// Wrap `inner`, yielding to the executor `yields` times before
        /// resolving. `yields == 0` behaves like [`BlockingHttpAdapter`].
        pub fn new(inner: T, yields: u32) -> Self {
            Self { inner, yields }
        }
    }

    impl<T: blocking::ClawHttp> ClawHttp for YieldingHttpAdapter<T> {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: super::Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                for _ in 0..self.yields {
                    if cancel.is_cancelled() {
                        return Err(HttpError::Aborted);
                    }
                    yield_once().await;
                }
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                // The final transfer step is the blocking inner call.
                let never = AtomicBool::new(false);
                let result = self.inner.post_json(request, &never);
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                result
            })
        }
    }
}

#[cfg(feature = "httpmock")]
pub use httpmock::{
    BlockingHttpAdapter, CapturingHttp, FailingHttp, NeverHttp, NoopHttp, ScriptStep, ScriptedHttp,
    SharedScriptHttp, YieldingHttpAdapter,
};

#[cfg(feature = "realhttp")]
mod realhttp {
    use std::time::Duration;

    use super::{
        cancel_on_poll, Cancel, ClawHttp, HttpError, HttpJsonRequest, HttpRequestFailure,
        HttpResponse, HttpResponseFuture, HttpStatusCode,
    };

    /// Host [`ClawHttp`] backed by an **async** `reqwest::Client`.
    ///
    /// The native non-blocking counterpart of [`blocking::RealHttp`]: it
    /// issues a genuinely concurrent request instead of blocking the polling
    /// task, so multiple in-flight calls progress together. It honours
    /// `request.auth` (bearer / API key / none), forwards extra
    /// headers, and treats any 2xx as success. Cancellation is handled by
    /// [`ClawHttp::post_json`] by dropping the in-flight reqwest future.
    ///
    /// Driver requirement: `reqwest`'s futures poll against **tokio**'s IO
    /// reactor, so this backend must be driven by a tokio runtime
    /// (`Runtime::block_on` or a tokio test runtime) — *not* by the cooperative
    /// `embedded-executor` used for the device-model `YieldingHttpAdapter`
    /// futures (those have no reactor). This keeps it strictly a host backend
    /// (CLIs, integration tests); on-device async HTTP uses the `esp_http_client`
    /// driver in `claw_sys` instead.
    ///
    /// The `reqwest::Client` pools connections, so construct one and reuse it.
    #[derive(Debug, Clone, Default)]
    pub struct RealHttp {
        client: reqwest::Client,
        user_agent: Option<String>,
    }

    impl RealHttp {
        /// A client with no extra `User-Agent`.
        pub fn new() -> Self {
            Self::default()
        }

        /// A client that sends the given `User-Agent` on every request.
        pub fn with_user_agent(user_agent: impl Into<String>) -> Self {
            Self {
                client: reqwest::Client::new(),
                user_agent: Some(user_agent.into()),
            }
        }
    }

    impl ClawHttp for RealHttp {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            let transfer: HttpResponseFuture<'a> = Box::pin(async move {
                let mut builder = self
                    .client
                    .post(request.url)
                    .header("Content-Type", "application/json")
                    .timeout(Duration::from_millis(u64::from(request.timeout_ms)))
                    .body(request.body.to_string());

                if let Some(user_agent) = &self.user_agent {
                    builder = builder.header("User-Agent", user_agent);
                }

                if let Some((name, value)) = request.auth.header() {
                    builder = builder.header(name, value);
                }
                for header in request.headers {
                    builder = builder.header(header.name, header.value);
                }

                let response = builder.send().await.map_err(|error| {
                    HttpError::RequestFailed(HttpRequestFailure::transport(error.to_string()))
                })?;
                let status_code = HttpStatusCode::new(response.status().as_u16());
                let body = response.text().await.map_err(|error| {
                    HttpError::RequestFailed(HttpRequestFailure::transport(error.to_string()))
                })?;

                if !status_code.is_success() {
                    return Err(HttpError::UnexpectedStatus {
                        status: status_code,
                        message: format!("HTTP {status_code}: {body}"),
                    });
                }
                Ok(HttpResponse { status_code, body })
            });
            cancel_on_poll(transfer, cancel)
        }
    }
}

#[cfg(feature = "realhttp")]
pub use realhttp::RealHttp;
