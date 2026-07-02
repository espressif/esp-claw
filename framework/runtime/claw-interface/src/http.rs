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

/// Networking injection point for the LLM backends.
///
/// Thread-safety is **not** a property of this trait: it carries no `Send`/`Sync`
/// supertrait bound. An implementation may hold a single, non-thread-safe
/// connection handle (e.g. an `esp_http_client` handle reused across calls — see
/// the espidf driver) and mutate it through `&mut self`. Concurrency is the
/// *caller's* decision: a transport driven by one task needs no synchronization;
/// a transport shared across tasks must be wrapped at that sharing point (e.g.
/// `Mutex<ClawApi<H>>`). With static dispatch the `Send`/`Sync` auto traits then
/// flow from the concrete implementor and are required only where threads cross.
pub trait ClawHttp {
    /// Equivalent to `claw_llm_http_post_json`. Returns the body on HTTP 2xx,
    /// otherwise an [`HttpError`]. `abort` is polled cooperatively; when set the
    /// request is cancelled (mirrors the C `volatile bool *abort_flag`).
    ///
    /// Takes `&mut self` so an implementation can lazily open and then reuse a
    /// persistent connection handle across calls (keep-alive) without interior
    /// mutability or locking.
    fn post_json(
        &mut self,
        request: &HttpJsonRequest,
        abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError>;
}

/// Boxed future returned by [`ClawHttpAsync::post_json`].
///
/// Boxed (instead of an `async fn` in the trait) so `ClawHttpAsync` stays object
/// safe. The future borrows `self` mutably, the request, and the cancellation token, so
/// it cannot outlive any of them.
pub type HttpResponseFuture<'a> =
    Pin<Box<dyn Future<Output = Result<HttpResponse, HttpError>> + 'a>>;

/// Cooperative cancellation token for [`ClawHttpAsync`].
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

/// Async counterpart of [`ClawHttp`], driven by a cooperative executor instead
/// of blocking the calling task for the whole request.
///
/// On ESP-IDF this maps onto `esp_http_client`'s non-blocking mode
/// (`config.is_async = true`): each poll runs one `esp_http_client_perform`
/// step and yields (`Poll::Pending`) while the call reports
/// `ESP_ERR_HTTP_EAGAIN`, letting other tasks run between steps.
///
/// # Implementing
///
/// Thread-safety is intentionally not a property of this base trait. Host
/// transports such as `reqwest::Client` may be `Send + Sync`, while embedded
/// transports may be task-local because they advance raw driver handles across
/// polls. Require `Send`/`Sync` at the caller boundary that actually crosses
/// threads. The method takes `&mut self` so an implementation can reuse one
/// persistent client handle while the borrow prevents another request from
/// using that handle concurrently.
///
/// Implementors own cancellation semantics in [`post_json`](ClawHttpAsync::post_json).
/// This keeps driver-specific cleanup at the implementation boundary: an
/// ESP-IDF implementation can cancel its active `esp_http_client` request,
/// while a host implementation can drop a reqwest future.
///
/// # Examples
///
/// A minimal in-memory transport, driven by a hand-rolled `block_on` (on device
/// a cooperative executor polls the future instead):
///
/// ```
/// use claw_interface::{
///     Cancel, ClawHttpAsync, HttpAuth, HttpJsonRequest, HttpResponse, HttpResponseFuture,
///     HttpStatusCode,
/// };
/// use core::future::Future;
/// use core::sync::atomic::AtomicBool;
/// use core::task::{Context, Poll};
/// use std::sync::Arc;
/// use std::task::{Wake, Waker};
///
/// // Implement `post_json`, including cancellation checks.
/// struct Echo;
/// impl ClawHttpAsync for Echo {
///     fn post_json<'a>(
///         &'a mut self,
///         request: &'a HttpJsonRequest<'a>,
///         cancel: Cancel<'a>,
///     ) -> HttpResponseFuture<'a> {
///         let body = request.body.to_string();
///         Box::pin(async move {
///             if cancel.is_cancelled() {
///                 return Err(claw_interface::HttpError::Aborted);
///             }
///             Ok(HttpResponse { status_code: HttpStatusCode::OK, body })
///         })
///     }
/// }
///
/// struct Noop;
/// impl Wake for Noop {
///     fn wake(self: Arc<Self>) {}
/// }
///
/// fn block_on<F: Future>(future: F) -> F::Output {
///     let mut future = Box::pin(future);
///     let waker = Waker::from(Arc::new(Noop));
///     let mut context = Context::from_waker(&waker);
///     loop {
///         if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
///             return value;
///         }
///     }
/// }
///
/// let request = HttpJsonRequest {
///     url: "https://example.test",
///     body: r#"{"hi":true}"#,
///     auth: HttpAuth::None,
///     timeout_ms: 1_000,
///     headers: &[],
/// };
///
/// // Not cancelled: resolves to the transfer's result.
/// let mut echo = Echo;
/// let flag = AtomicBool::new(false);
/// let response = block_on(echo.post_json(&request, Cancel::new(&flag))).unwrap();
/// assert_eq!(response.status_code, HttpStatusCode::OK);
/// assert_eq!(response.body, r#"{"hi":true}"#);
///
/// // A pre-set flag cancels before the transfer runs.
/// let cancelled = AtomicBool::new(true);
/// assert!(block_on(echo.post_json(&request, Cancel::new(&cancelled))).is_err());
/// ```
pub trait ClawHttpAsync {
    /// Async equivalent of [`ClawHttp::post_json`].
    ///
    /// Implementations must observe `cancel` cooperatively and return
    /// [`HttpError::Aborted`] when cancellation wins. A non-blocking driver should
    /// release any in-flight request state when the returned future is dropped,
    /// because callers may cancel by dropping the future as well as by setting the
    /// token.
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a>;

    /// Async JSON GET for read-only capability clients.
    ///
    /// Implementations that only support POST keep the default rejection. Callers
    /// should surface it as a capability/tool invocation error rather than
    /// silently changing methods.
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
    use core::cell::RefCell;
    use core::future::Future;
    use core::pin::Pin;
    use core::task::{Context, Poll};
    use std::collections::VecDeque;
    use std::sync::atomic::AtomicBool;
    use std::sync::{Arc, Mutex, MutexGuard};

    use super::{
        ClawHttp, ClawHttpAsync, HttpError, HttpJsonRequest, HttpRequestFailure, HttpResponse,
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

    impl ClawHttp for ScriptedHttp {
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
    impl ClawHttp for Arc<ScriptedHttp> {
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
                .map(|body| serde_json::from_str(body).unwrap_or(serde_json::Value::Null))
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

    impl ClawHttp for CapturingHttp {
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
    impl ClawHttp for Arc<CapturingHttp> {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.serve(request)
        }
    }

    thread_local! {
        /// The script every [`SharedScriptHttp::default`] on this thread shares.
        /// Installed by [`SharedScriptHttp::install`]; read once at construction.
        static SHARED_SCRIPT: RefCell<Option<Arc<Mutex<VecDeque<ScriptStep>>>>> =
            const { RefCell::new(None) };
    }

    /// A [`Default`]-constructible scripted transport for systems that mint their
    /// own clients and choose the transport by *type* (e.g. `FsAgentFactory<F, H>`
    /// / `AgentSystemBuilder<F, H>` with `H = SharedScriptHttp`).
    ///
    /// A plain [`ScriptedHttp`] can't be injected into those, because the system
    /// constructs each `H::default()` internally. `SharedScriptHttp` bridges the
    /// gap: a test calls [`install`](Self::install) once, then **every**
    /// `SharedScriptHttp::default()` constructed on that thread shares the one
    /// script and pops from it in call order — reproducing the single-shared-script
    /// behavior of an injected [`ScriptedHttp`]. Strict: panics if called more
    /// times than scripted, or if no script was installed.
    #[derive(Clone)]
    pub struct SharedScriptHttp {
        steps: Option<Arc<Mutex<VecDeque<ScriptStep>>>>,
    }

    impl SharedScriptHttp {
        /// Install the script shared by every later `SharedScriptHttp::default()`
        /// on the current thread. Call once before building the system under test.
        pub fn install(bodies: impl IntoIterator<Item = impl Into<String>>) {
            let steps = Arc::new(Mutex::new(into_steps(bodies)));
            SHARED_SCRIPT.with(|cell| *cell.borrow_mut() = Some(steps));
        }

        /// Drop the installed script (so a later `default()` with no script fails
        /// loudly rather than replaying a stale one).
        pub fn clear() {
            SHARED_SCRIPT.with(|cell| *cell.borrow_mut() = None);
        }
    }

    impl Default for SharedScriptHttp {
        fn default() -> Self {
            Self {
                steps: SHARED_SCRIPT.with(|cell| cell.borrow().clone()),
            }
        }
    }

    impl ClawHttp for SharedScriptHttp {
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

    impl ClawHttp for FailingHttp {
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

    impl ClawHttp for NoopHttp {
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

    impl ClawHttp for NeverHttp {
        fn post_json(
            &mut self,
            _request: &HttpJsonRequest,
            _abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            panic!("NeverHttp: the LLM must not be called in this test");
        }
    }

    // -----------------------------------------------------------------------
    // Async-seam adapters: turn a blocking `ClawHttp` into a `ClawHttpAsync`.
    // Host-only, so they live beside the blocking doubles instead of in the
    // platform-free seam, and never enter the device/default build.
    // -----------------------------------------------------------------------

    /// Adapts any blocking [`ClawHttp`] into a [`ClawHttpAsync`] by running the
    /// request to completion in a single poll.
    ///
    /// Intended for the host (CLIs, tests) where the blocking transports — the
    /// scripted doubles above, `RealHttp` — already exist and a real
    /// cooperative executor is unnecessary. It is **not** appropriate on-device:
    /// the wrapped call blocks the polling task for the whole request, defeating
    /// the purpose of the async seam (use the native `esp_http_client` driver
    /// there instead).
    pub struct BlockingClawHttpAsync<T>(T);

    impl<T> BlockingClawHttpAsync<T> {
        /// Wrap a blocking [`ClawHttp`] for the async seam.
        pub fn new(inner: T) -> Self {
            Self(inner)
        }
    }

    impl<T: Default> Default for BlockingClawHttpAsync<T> {
        fn default() -> Self {
            Self(T::default())
        }
    }

    impl<T: ClawHttp> ClawHttpAsync for BlockingClawHttpAsync<T> {
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

    /// Adapts any blocking [`ClawHttp`] into a [`ClawHttpAsync`] that yields to
    /// the executor `yields` times **before** running the wrapped request to
    /// completion.
    ///
    /// This is a host/test simulation of a genuinely non-blocking transport: it
    /// reproduces the multi-poll, `Poll::Pending`-between-steps shape of the
    /// on-device `esp_http_client` driver (which yields on every
    /// `ESP_ERR_HTTP_EAGAIN`) without needing real hardware. Use it to drive a
    /// cooperative executor's poll/wake loop against the [`ClawHttpAsync`] seam:
    /// unlike [`BlockingClawHttpAsync`] (which resolves in a single poll), its
    /// future returns `Poll::Pending` `yields` times first.
    ///
    /// Like [`BlockingClawHttpAsync`], the final transfer step still calls the
    /// blocking [`ClawHttp`], so it is **not** appropriate on-device.
    pub struct YieldingClawHttpAsync<T> {
        inner: T,
        yields: u32,
    }

    impl<T> YieldingClawHttpAsync<T> {
        /// Wrap `inner`, yielding to the executor `yields` times before
        /// resolving. `yields == 0` behaves like [`BlockingClawHttpAsync`].
        pub fn new(inner: T, yields: u32) -> Self {
            Self { inner, yields }
        }

        /// Borrow the wrapped blocking transport. Test-only: the in-crate tests
        /// inspect the wrapped double's call counter.
        #[cfg(test)]
        pub(crate) fn inner(&self) -> &T {
            &self.inner
        }
    }

    impl<T: ClawHttp> ClawHttpAsync for YieldingClawHttpAsync<T> {
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
    BlockingClawHttpAsync, CapturingHttp, FailingHttp, NeverHttp, NoopHttp, ScriptStep,
    ScriptedHttp, SharedScriptHttp, YieldingClawHttpAsync,
};

#[cfg(feature = "realhttp")]
mod realhttp {
    use core::sync::atomic::{AtomicBool, Ordering};
    use std::time::Duration;

    use super::{
        ClawHttp, HttpError, HttpJsonRequest, HttpRequestFailure, HttpResponse, HttpStatusCode,
    };

    /// Host [`ClawHttp`] backed by a blocking `reqwest` client.
    ///
    /// The single real transport for host CLIs and live/integration tests:
    /// honours `request.auth`, forwards extra headers, polls `abort` before
    /// sending, and treats any 2xx as success.
    /// An optional `User-Agent` lets a test route requests by client identity.
    #[derive(Debug, Clone, Default)]
    pub struct RealHttp {
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
                user_agent: Some(user_agent.into()),
            }
        }
    }

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

#[cfg(feature = "realhttp")]
pub use realhttp::RealHttp;

#[cfg(feature = "realhttp")]
mod realhttp_async {
    use std::time::Duration;

    use super::{
        cancel_on_poll, Cancel, ClawHttpAsync, HttpError, HttpJsonRequest, HttpRequestFailure,
        HttpResponse, HttpResponseFuture, HttpStatusCode,
    };

    /// Host [`ClawHttpAsync`] backed by an **async** `reqwest::Client`.
    ///
    /// The native non-blocking counterpart of the blocking `RealHttp`: it
    /// issues a genuinely concurrent request instead of blocking the polling
    /// task, so multiple in-flight calls progress together. It honours
    /// `request.auth` (bearer / API key / none), forwards extra
    /// headers, and treats any 2xx as success. Cancellation is handled by
    /// [`ClawHttpAsync::post_json`] by dropping the in-flight reqwest future.
    ///
    /// Driver requirement: `reqwest`'s futures poll against **tokio**'s IO
    /// reactor, so this backend must be driven by a tokio runtime
    /// (`Runtime::block_on` / `#[tokio::test]`) — *not* by the cooperative
    /// `embedded-executor` used for the device-model `YieldingClawHttpAsync`
    /// futures (those have no reactor). This keeps it strictly a host backend
    /// (CLIs, integration tests); on-device async HTTP uses the `esp_http_client`
    /// driver in `claw_sys` instead.
    ///
    /// The `reqwest::Client` pools connections, so construct one and reuse it.
    #[derive(Debug, Clone, Default)]
    pub struct RealHttpAsync {
        client: reqwest::Client,
        user_agent: Option<String>,
    }

    impl RealHttpAsync {
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

    impl ClawHttpAsync for RealHttpAsync {
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
pub use realhttp_async::RealHttpAsync;

/// Shared host-only test doubles + helpers for the async-seam test modules
/// below. Kept in one place so the hand-rolled `block_on` tests and the
/// `embedded-executor` integration tests exercise the same transports.
///
/// Gated on `httpmock` (the home of the `ClawHttp` test doubles) because the
/// async-seam tests drive [`YieldingClawHttpAsync`], which lives behind that
/// feature. Run them with `--features httpmock`.
#[cfg(all(test, feature = "httpmock"))]
mod async_test_support {
    use super::{
        ClawHttp, HttpAuth, HttpError, HttpJsonRequest, HttpRequestFailure, HttpResponse,
        HttpStatusCode,
    };
    use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

    /// `ClawHttp` that echoes the request body back with a fixed status code and
    /// counts how many times the (blocking) transport was actually invoked.
    pub struct EchoStatus {
        pub status: HttpStatusCode,
        pub calls: AtomicU32,
    }

    impl EchoStatus {
        pub fn new(status: u16) -> Self {
            Self {
                status: HttpStatusCode::new(status),
                calls: AtomicU32::new(0),
            }
        }
    }

    impl ClawHttp for EchoStatus {
        fn post_json(
            &mut self,
            request: &HttpJsonRequest,
            abort: &AtomicBool,
        ) -> Result<HttpResponse, HttpError> {
            self.calls.fetch_add(1, Ordering::Relaxed);
            if abort.load(Ordering::Acquire) {
                return Err(HttpError::Aborted);
            }
            Ok(HttpResponse {
                status_code: self.status,
                body: request.body.to_string(),
            })
        }
    }

    /// `ClawHttp` that always fails the round with a transport error.
    pub struct FailingStatus;

    impl ClawHttp for FailingStatus {
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

    pub fn request<'a>(url: &'a str, body: &'a str) -> HttpJsonRequest<'a> {
        HttpJsonRequest {
            url,
            body,
            auth: HttpAuth::None,
            timeout_ms: 1_000,
            headers: &[],
        }
    }
}

#[cfg(all(test, feature = "httpmock"))]
mod async_seam_tests {
    use super::async_test_support::{request, EchoStatus, FailingStatus};
    use super::{
        BlockingClawHttpAsync, Cancel, ClawHttpAsync, HttpError, HttpStatusCode,
        YieldingClawHttpAsync,
    };
    use core::future::Future;
    use core::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::task::{Context, Poll, Wake};

    /// Noop waker: our reference futures resolve in a bounded number of polls,
    /// so a spin-driven `block_on` needs no real wakeups.
    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    /// Drives `future` to completion by spinning, returning the output and the
    /// number of `poll` calls it took (so a test can assert a future actually
    /// yielded `Poll::Pending` rather than resolving in one shot).
    fn block_on_counting<F: Future>(future: F) -> (F::Output, u32) {
        let mut future = Box::pin(future);
        let waker = Arc::new(NoopWake).into();
        let mut context = Context::from_waker(&waker);
        let mut polls = 0_u32;
        loop {
            polls += 1;
            if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
                return (output, polls);
            }
        }
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        block_on_counting(future).0
    }

    #[test]
    fn blocking_adapter_drives_clawhttp_through_async_seam() {
        let mut transport = BlockingClawHttpAsync::new(EchoStatus::new(200));
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "ping");
        let response = block_on(transport.post_json(&request, Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert_eq!(response.body, "ping");
    }

    #[test]
    fn blocking_adapter_is_object_safe() {
        // The async seam stays object-safe; a mutable trait object preserves the
        // single in-flight request guarantee.
        let mut transport: Box<dyn ClawHttpAsync> =
            Box::new(BlockingClawHttpAsync::new(EchoStatus::new(204)));
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "{}");
        let response = block_on(transport.post_json(&request, Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::NO_CONTENT);
    }

    #[test]
    fn blocking_adapter_resolves_in_a_single_poll() {
        let mut transport = BlockingClawHttpAsync::new(EchoStatus::new(200));
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "{}");
        let (response, polls) =
            block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
        assert!(response.is_ok());
        assert_eq!(polls, 1, "blocking adapter must not yield");
    }

    #[test]
    fn yielding_adapter_yields_before_resolving() {
        let inner = EchoStatus::new(200);
        let mut transport = YieldingClawHttpAsync::new(inner, 3);
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "payload");
        let (response, polls) =
            block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
        let response = response.expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert_eq!(response.body, "payload");
        // 3 pending yields + 1 final resolving poll.
        assert_eq!(polls, 4);
        // The wrapped blocking transport is invoked exactly once, on the last poll.
        assert_eq!(transport.inner().calls.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn yielding_adapter_zero_yields_resolves_in_one_poll() {
        // Boundary: `yields == 0` must collapse to blocking-adapter behavior —
        // a single poll, inner transport invoked exactly once, no `Pending`.
        let mut transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 0);
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "edge");
        let (response, polls) =
            block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
        let response = response.expect("ok");
        assert_eq!(response.body, "edge");
        assert_eq!(polls, 1, "yields=0 must not yield Pending");
        assert_eq!(transport.inner().calls.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn yielding_adapter_propagates_transport_error() {
        let mut transport = YieldingClawHttpAsync::new(FailingStatus, 2);
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "{}");
        let error = block_on(transport.post_json(&request, Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)));
    }

    #[test]
    fn yielding_adapter_honors_abort() {
        let mut transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 2);
        let abort = AtomicBool::new(true);
        let request = request("https://example.test", "{}");
        let error = block_on(transport.post_json(&request, Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::Aborted));
    }
}

/// Integration tests proving the [`ClawHttpAsync`] seam is driven correctly by a
/// real cooperative `no_std`-style executor (`embedded-executor`'s
/// `AllocExecutor`), not just by the hand-rolled spin `block_on` above. This is
/// the on-device execution model: a single-threaded executor polling `!Send`
/// futures whose wakers re-arm via [`Wake`](embedded_executor::Wake).
///
/// # Sleep choice
///
/// `AllocExecutor::run` calls `Sleep::sleep` after every dequeued item while the
/// registry is non-empty, expecting a `Wake` to have armed the sleep. That holds
/// for a future that returns `Poll::Pending` *and* re-arms its waker (our
/// [`YieldingClawHttpAsync`] does, mirroring the on-device EAGAIN loop). It does
/// **not** hold the moment a task resolves while a sibling task is still
/// registered: nothing arms the sleep, so the bundled `SpinSleep` (a blocking
/// spin-until-woken flag) would deadlock. These tests therefore use a no-op
/// `Sleep` (busy-poll), exactly as `embedded-executor`'s own tests do — the
/// correct choice for a single-threaded executor whose only wakeups come from
/// the futures it is already polling.
///
/// Gated on `httpmock` (run with `--features httpmock`): the tests drive
/// [`YieldingClawHttpAsync`], which lives behind that feature.
#[cfg(all(test, feature = "httpmock"))]
mod embedded_executor_tests {
    use super::async_test_support::{request, EchoStatus, FailingStatus};
    use super::{
        BlockingClawHttpAsync, Cancel, ClawHttpAsync, HttpError, HttpResponse, HttpStatusCode,
        YieldingClawHttpAsync,
    };
    use core::sync::atomic::{AtomicBool, Ordering};
    use std::cell::RefCell;
    use std::rc::Rc;

    use embedded_executor::{AllocExecutor, Sleep, Wake};
    use lock_api::{GuardSend, RawMutex};

    /// Minimal host spinlock so `AllocExecutor` has a `RawMutex`. On device this
    /// would disable/re-enable interrupts; a single-threaded host test only ever
    /// sees an uncontended lock, so a plain spin is sufficient.
    struct RawSpinlock(AtomicBool);

    // SAFETY: a compare-exchange spinlock is a sound `RawMutex`: `lock` blocks
    // until it atomically transitions the flag false -> true, `unlock` releases
    // it, and the Acquire/Release ordering pairs the critical sections.
    unsafe impl RawMutex for RawSpinlock {
        #[allow(clippy::declare_interior_mutable_const)]
        const INIT: RawSpinlock = RawSpinlock(AtomicBool::new(false));
        type GuardMarker = GuardSend;

        fn lock(&self) {
            while !self.try_lock() {
                core::hint::spin_loop();
            }
        }

        fn try_lock(&self) -> bool {
            self.0
                .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
        }

        unsafe fn unlock(&self) {
            self.0.store(false, Ordering::Release);
        }
    }

    /// No-op `Sleep`: the executor busy-polls its ready queue. See the module
    /// doc for why a blocking `Sleep` (e.g. the bundled `SpinSleep`) is wrong for
    /// this single-threaded, self-waking model.
    #[derive(Clone, Copy, Default)]
    struct NopSleep;
    impl Sleep for NopSleep {
        fn sleep(&self) {}
    }
    impl Wake for NopSleep {
        fn wake(&self) {}
    }

    type TestExecutor<'a> = AllocExecutor<'a, RawSpinlock, NopSleep>;

    /// Spawn `transport.post_json(...)` onto a fresh executor, run it to
    /// completion, and return the captured result. `transport`, the request, and
    /// the abort flag are owned by the spawned future so it stays `'static`.
    fn run_async<T>(
        mut transport: T,
        body: &'static str,
        abort: AtomicBool,
    ) -> Result<HttpResponse, HttpError>
    where
        T: ClawHttpAsync + 'static,
    {
        let sink: Rc<RefCell<Option<Result<HttpResponse, HttpError>>>> =
            Rc::new(RefCell::new(None));
        let result_sink = Rc::clone(&sink);

        let mut executor: TestExecutor = AllocExecutor::new();
        executor.spawn(async move {
            let request = request("https://example.test", body);
            let result = transport.post_json(&request, Cancel::new(&abort)).await;
            *result_sink.borrow_mut() = Some(result);
        });
        executor.run();

        let captured = sink.borrow_mut().take();
        captured.expect("executor finished without capturing a result")
    }

    #[test]
    fn executor_drives_blocking_transport_to_completion() {
        let transport = BlockingClawHttpAsync::new(EchoStatus::new(200));
        let response = run_async(transport, "hello", AtomicBool::new(false)).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert_eq!(response.body, "hello");
    }

    #[test]
    fn executor_drives_yielding_transport_to_completion() {
        // 5 cooperative yields means the executor must poll, observe
        // `Poll::Pending`, honor the re-armed waker, and re-poll five times
        // before the request resolves — exactly the on-device EAGAIN loop.
        let transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 5);
        let response = run_async(transport, "hello", AtomicBool::new(false)).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert_eq!(response.body, "hello");
    }

    #[test]
    fn executor_propagates_transport_error() {
        let transport = YieldingClawHttpAsync::new(FailingStatus, 3);
        let error = run_async(transport, "{}", AtomicBool::new(false)).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)));
    }

    #[test]
    fn executor_honors_abort() {
        let transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 2);
        let error = run_async(transport, "{}", AtomicBool::new(true)).unwrap_err();
        assert!(matches!(error, HttpError::Aborted));
    }

    #[test]
    fn executor_interleaves_concurrent_requests() {
        // Two async requests with different yield counts share one executor.
        // Both must complete, proving the executor cooperatively interleaves
        // multiple `ClawHttpAsync` futures rather than blocking on the first.
        let captures: Rc<RefCell<Vec<(u32, HttpStatusCode)>>> = Rc::new(RefCell::new(Vec::new()));

        let mut executor: TestExecutor = AllocExecutor::new();
        for (id, yields, status) in [(1_u32, 4_u32, 201_u16), (2, 1, 202)] {
            let sink = Rc::clone(&captures);
            executor.spawn(async move {
                let mut transport = YieldingClawHttpAsync::new(EchoStatus::new(status), yields);
                let abort = AtomicBool::new(false);
                let request = request("https://example.test", "body");
                if let Ok(response) = transport.post_json(&request, Cancel::new(&abort)).await {
                    sink.borrow_mut().push((id, response.status_code));
                }
            });
        }
        executor.run();

        let mut results = captures.borrow().clone();
        results.sort_by_key(|(id, _)| *id);
        assert_eq!(
            results,
            vec![(1, HttpStatusCode::new(201)), (2, HttpStatusCode::new(202))]
        );
    }
}

/// End-to-end tests for the native async host backend [`RealHttpAsync`], driven
/// by a real **tokio** runtime against a one-shot loopback HTTP server. This
/// exercises the actual `reqwest` async client + tokio reactor — the host's
/// genuinely non-blocking transport — rather than a blocking bridge.
#[cfg(all(test, feature = "realhttp"))]
mod realhttp_async_tests {
    use super::{
        Cancel, ClawHttpAsync, HttpAuth, HttpError, HttpHeader, HttpJsonRequest, HttpStatusCode,
        RealHttpAsync,
    };
    use core::sync::atomic::AtomicBool;
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::sync::mpsc;
    use std::thread::JoinHandle;
    use std::time::Duration;

    /// Spawn a one-shot loopback HTTP/1.1 server that replies with `status_line`
    /// + `body` and reports the raw request bytes it received over a channel.
    /// Returns the POST URL, the request receiver, and the server thread handle.
    fn oneshot_server(
        status_line: &'static str,
        body: impl Into<String>,
    ) -> (String, mpsc::Receiver<String>, JoinHandle<()>) {
        let body = body.into();
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("local addr");
        let (tx, rx) = mpsc::channel();
        let handle = std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut buf = [0u8; 4096];
                let read = stream.read(&mut buf).unwrap_or(0);
                let _ = tx.send(String::from_utf8_lossy(&buf[..read]).into_owned());
                let response = format!(
                    "HTTP/1.1 {status_line}\r\nContent-Type: application/json\r\n\
                     Content-Length: {}\r\nConnection: close\r\n\r\n{body}",
                    body.len(),
                );
                let _ = stream.write_all(response.as_bytes());
                let _ = stream.flush();
            }
        });
        (format!("http://{addr}/v1/chat/completions"), rx, handle)
    }

    /// A loopback URL whose listener has been dropped, so connecting is refused.
    fn refused_url() -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("local addr");
        drop(listener);
        format!("http://{addr}/")
    }

    /// A server that accepts + reads the request, then holds the connection open
    /// without replying for `hold_ms`, forcing a client-side timeout. The thread
    /// is detached (we don't join) so the test isn't blocked on the hold.
    fn stalling_server(hold_ms: u64) -> String {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
        let addr = listener.local_addr().expect("local addr");
        std::thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let mut buf = [0u8; 1024];
                let _ = stream.read(&mut buf);
                std::thread::sleep(Duration::from_millis(hold_ms));
                // Drop the stream (close) without ever sending a response.
            }
        });
        format!("http://{addr}/")
    }

    /// Drive a future on a current-thread tokio runtime (the reactor `reqwest`
    /// polls against). Non-`Send` futures are fine under `block_on`.
    fn block_on<F: core::future::Future>(future: F) -> F::Output {
        tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .expect("tokio runtime")
            .block_on(future)
    }

    /// A minimal POST request with defaults (no auth, no extra headers, 5s).
    fn req<'a>(url: &'a str, body: &'a str) -> HttpJsonRequest<'a> {
        HttpJsonRequest {
            url,
            body,
            auth: HttpAuth::None,
            timeout_ms: 5_000,
            headers: &[],
        }
    }

    #[test]
    fn async_reqwest_roundtrip_sends_auth_and_parses_body() {
        let (url, rx, handle) = oneshot_server("200 OK", r#"{"choices":[]}"#);
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let headers = [HttpHeader {
            name: "X-Trace",
            value: "abc",
        }];
        let request = HttpJsonRequest {
            url: &url,
            body: r#"{"model":"x"}"#,
            auth: HttpAuth::Bearer("sk-test"),
            timeout_ms: 5_000,
            headers: &headers,
        };

        let response = block_on(http.post_json(&request, Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert_eq!(response.body, r#"{"choices":[]}"#);

        // The async reqwest client really sent our method, auth, headers, body.
        // (reqwest/hyper writes header names lowercase; values keep their case.)
        let raw = rx.recv().expect("server captured request");
        assert!(raw.starts_with("POST /v1/chat/completions "), "raw: {raw}");
        assert!(raw.contains("authorization: Bearer sk-test"), "raw: {raw}");
        assert!(raw.contains("x-trace: abc"), "raw: {raw}");
        assert!(raw.contains(r#"{"model":"x"}"#), "raw: {raw}");
        handle.join().expect("server thread");
    }

    // ---- status-code range boundary (`RealHttpAsync` treats `200..300` as ok) --

    #[test]
    fn async_reqwest_status_204_no_content_is_success() {
        let (url, _rx, handle) = oneshot_server("204 No Content", "");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::NO_CONTENT);
        assert_eq!(response.body, "");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_status_299_upper_edge_is_success() {
        let (url, _rx, handle) = oneshot_server("299 Almost", "edge");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::new(299));
        assert_eq!(response.body, "edge");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_status_300_just_outside_is_unexpected() {
        // 300 is the exclusive upper bound of `200..300`; no `Location`, so the
        // default redirect policy does not follow it.
        let (url, _rx, handle) = oneshot_server("300 Multiple Choices", "nope");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
        match error {
            HttpError::UnexpectedStatus { status, message } => {
                assert_eq!(status, HttpStatusCode::new(300));
                assert!(message.contains("300"), "message: {message}");
                assert!(message.contains("nope"), "message: {message}");
            }
            other => panic!("expected UnexpectedStatus, got {other:?}"),
        }
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_status_503_is_unexpected_with_body() {
        let (url, _rx, handle) = oneshot_server("503 Service Unavailable", r#"{"error":"down"}"#);
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
        match error {
            HttpError::UnexpectedStatus { status, message } => {
                assert_eq!(status, HttpStatusCode::new(503));
                assert!(message.contains("503"), "message: {message}");
                assert!(message.contains("down"), "message: {message}");
            }
            other => panic!("expected UnexpectedStatus, got {other:?}"),
        }
        handle.join().expect("server thread");
    }

    // ---- auth-header variants ----------------------------------------------

    #[test]
    fn async_reqwest_api_key_auth_uses_api_key_header() {
        let (url, rx, handle) = oneshot_server("200 OK", "{}");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            auth: HttpAuth::ApiKey("secret"),
            timeout_ms: 5_000,
            headers: &[],
        };
        block_on(http.post_json(&request, Cancel::new(&abort))).expect("ok");
        let raw = rx.recv().expect("captured");
        assert!(raw.contains("x-api-key: secret"), "raw: {raw}");
        assert!(!raw.to_lowercase().contains("authorization:"), "raw: {raw}");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_auth_none_omits_authorization_even_with_key() {
        let (url, rx, handle) = oneshot_server("200 OK", "{}");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            auth: HttpAuth::None,
            timeout_ms: 5_000,
            headers: &[],
        };
        block_on(http.post_json(&request, Cancel::new(&abort))).expect("ok");
        let raw = rx.recv().expect("captured");
        let lower = raw.to_lowercase();
        assert!(!lower.contains("authorization:"), "raw: {raw}");
        assert!(!raw.contains("secret"), "raw: {raw}");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_auth_none_omits_authorization() {
        let (url, rx, handle) = oneshot_server("200 OK", "{}");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            auth: HttpAuth::None,
            timeout_ms: 5_000,
            headers: &[],
        };
        block_on(http.post_json(&request, Cancel::new(&abort))).expect("ok");
        let raw = rx.recv().expect("captured");
        assert!(!raw.to_lowercase().contains("authorization:"), "raw: {raw}");
        handle.join().expect("server thread");
    }

    // ---- body-size boundaries ----------------------------------------------

    #[test]
    fn async_reqwest_empty_200_body_is_ok() {
        let (url, _rx, handle) = oneshot_server("200 OK", "");
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert!(response.body.is_empty());
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_large_body_roundtrip() {
        // Larger than the server's request read buffer and any single TCP read,
        // so the client must accumulate the full Content-Length.
        let big = "a".repeat(64 * 1024);
        let (url, _rx, handle) = oneshot_server("200 OK", big.clone());
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, HttpStatusCode::OK);
        assert_eq!(response.body.len(), big.len());
        assert_eq!(response.body, big);
        handle.join().expect("server thread");
    }

    // ---- failure modes ------------------------------------------------------

    #[test]
    fn async_reqwest_honors_abort_before_send() {
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(true);
        let error =
            block_on(http.post_json(&req("http://127.0.0.1:9/never", "{}"), Cancel::new(&abort)))
                .unwrap_err();
        assert!(matches!(error, HttpError::Aborted));
    }

    #[test]
    fn async_reqwest_connection_refused_is_request_failed() {
        let url = refused_url();
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
    }

    #[test]
    fn async_reqwest_timeout_is_request_failed() {
        let url = stalling_server(2_000);
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            auth: HttpAuth::None,
            timeout_ms: 150, // fires well before the server's 2s hold
            headers: &[],
        };
        let error = block_on(http.post_json(&request, Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
    }

    #[test]
    fn async_reqwest_invalid_url_is_request_failed() {
        let mut http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error =
            block_on(http.post_json(&req("not a url", "{}"), Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
    }
}
