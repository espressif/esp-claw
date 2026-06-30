//! The `ClawHttp` networking injection trait.
//!
//! Replaces `claw_llm_http_transport.c`. The espidf wiring implements this over
//! `esp_http_client`; host tests provide canned responses.

use core::future::Future;
use core::pin::Pin;
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::Poll;

/// A single extra request header (`name: value`).
pub struct HttpHeader<'a> {
    pub name: &'a str,
    pub value: &'a str,
}

/// Parameters for a JSON POST, mirroring `claw_llm_http_json_request_t`.
pub struct HttpJsonRequest<'a> {
    pub url: &'a str,
    pub body: &'a str,
    /// `None` (or empty) disables the auth header.
    pub api_key: Option<&'a str>,
    /// `"bearer"` (default), `"api-key"`, or `"none"`.
    pub auth_type: Option<&'a str>,
    pub timeout_ms: u32,
    pub headers: &'a [HttpHeader<'a>],
}

/// A successful HTTP response (status 200), mirroring `claw_llm_http_response_t`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HttpResponse {
    pub status_code: i32,
    pub body: String,
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
    RequestFailed(String),
    /// Non-200 response; message matches C `parse_error_message_body` shape.
    #[error("{0}")]
    UnexpectedStatus(String),
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
    /// Equivalent to `claw_llm_http_post_json`. Returns the body on HTTP 200,
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
/// Boxed (instead of an `async fn` in the trait) so `ClawHttpAsync` stays
/// object-safe: the LLM stack shares its transport as `Arc<dyn ClawHttpAsync>`,
/// exactly like the blocking [`ClawHttp`] seam. The future borrows `self`, the
/// request, and the abort flag, so it cannot outlive any of them.
///
/// The future is intentionally **not** `Send`: the espidf driver advances an
/// `esp_http_client` handle (a raw pointer) across polls, so it must be polled
/// on the task that created it — the common single-task embedded executor model.
pub type HttpResponseFuture<'a> =
    Pin<Box<dyn Future<Output = Result<HttpResponse, HttpError>> + 'a>>;

/// Cooperative cancellation token for [`ClawHttpAsync`].
///
/// A thin, `Copy` wrapper over a caller-owned abort flag. Unlike a blocking
/// [`ClawHttp`] call (cancelled only by the in-band flag), the async seam
/// cancels *structurally*: when the flag is set, [`ClawHttpAsync::post_json`]
/// drops the in-flight transfer future — running its `Drop` and tearing down
/// the underlying client. Set the flag from any context (another task, an
/// interrupt) to request cancellation.
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
/// Implementors provide only [`transfer`](ClawHttpAsync::transfer) — the plain
/// happy-path request, with no cancellation logic. Cancellation is layered on
/// by the provided [`post_json`](ClawHttpAsync::post_json), which races the
/// transfer against the [`Cancel`] token and drops the transfer future on
/// cancellation (its `Drop` tears down any in-flight client).
///
/// # Examples
///
/// A minimal in-memory transport, driven by a hand-rolled `block_on` (on device
/// a cooperative executor polls the future instead):
///
/// ```
/// use claw_interface::{
///     Cancel, ClawHttpAsync, HttpJsonRequest, HttpResponse, HttpResponseFuture,
/// };
/// use core::future::Future;
/// use core::sync::atomic::AtomicBool;
/// use core::task::{Context, Poll};
/// use std::sync::Arc;
/// use std::task::{Wake, Waker};
///
/// // Implement only `transfer`; `post_json` (with cancellation) comes for free.
/// struct Echo;
/// impl ClawHttpAsync for Echo {
///     fn transfer<'a>(&'a self, request: &'a HttpJsonRequest<'a>) -> HttpResponseFuture<'a> {
///         let body = request.body.to_string();
///         Box::pin(async move { Ok(HttpResponse { status_code: 200, body }) })
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
///     api_key: None,
///     auth_type: None,
///     timeout_ms: 1_000,
///     headers: &[],
/// };
///
/// // Not cancelled: resolves to the transfer's result.
/// let flag = AtomicBool::new(false);
/// let response = block_on(Echo.post_json(&request, Cancel::new(&flag))).unwrap();
/// assert_eq!(response.status_code, 200);
/// assert_eq!(response.body, r#"{"hi":true}"#);
///
/// // A pre-set flag cancels before the transfer runs.
/// let cancelled = AtomicBool::new(true);
/// assert!(block_on(Echo.post_json(&request, Cancel::new(&cancelled))).is_err());
/// ```
pub trait ClawHttpAsync: Send + Sync {
    /// Run the request to completion, resolving to the body on HTTP 200 (or any
    /// 2xx for host backends), otherwise an [`HttpError`]. Implementations must
    /// **not** embed cancellation logic here — see [`post_json`] for that.
    ///
    /// The returned future must clean up its resources on `Drop`, since
    /// [`post_json`] cancels by dropping it mid-flight.
    ///
    /// [`post_json`]: ClawHttpAsync::post_json
    fn transfer<'a>(&'a self, request: &'a HttpJsonRequest<'a>) -> HttpResponseFuture<'a>;

    /// Async equivalent of [`ClawHttp::post_json`]. Races [`transfer`] against
    /// `cancel`: if cancellation fires first, the transfer future is dropped
    /// (cleaning up the in-flight request) and [`HttpError::Aborted`] is
    /// returned; otherwise it resolves to the transfer's result.
    ///
    /// [`transfer`]: ClawHttpAsync::transfer
    fn post_json<'a>(
        &'a self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(race_cancel(self.transfer(request), cancel))
    }
}

/// Drives `transfer` while checking `cancel` on every poll. On cancellation it
/// returns [`HttpError::Aborted`]; the captured `transfer` future is then
/// dropped by the caller's await machinery, running its `Drop`.
///
/// No executor primitives or external crates are needed: a non-blocking
/// `transfer` re-arms its own waker between steps (mirroring the device
/// `ESP_ERR_HTTP_EAGAIN` loop), so this `poll_fn` is re-polled each step and
/// re-checks `cancel` without a separate waker on the cancel side.
async fn race_cancel<'a>(
    mut transfer: HttpResponseFuture<'a>,
    cancel: Cancel<'a>,
) -> Result<HttpResponse, HttpError> {
    core::future::poll_fn(move |context| {
        if cancel.is_cancelled() {
            return Poll::Ready(Err(HttpError::Aborted));
        }
        transfer.as_mut().poll(context)
    })
    .await
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
        ClawHttp, ClawHttpAsync, HttpError, HttpJsonRequest, HttpResponse, HttpResponseFuture,
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
                status_code: 200,
                body,
            }),
            Err(message) => Err(HttpError::RequestFailed(message)),
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
            Err(HttpError::RequestFailed("simulated failure".into()))
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
            Err(HttpError::RequestFailed("noop".into()))
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
    pub struct BlockingClawHttpAsync<T>(Mutex<T>);

    impl<T> BlockingClawHttpAsync<T> {
        /// Wrap a blocking [`ClawHttp`] for the async seam.
        ///
        /// The inner transport is driven through `&mut self` ([`ClawHttp::post_json`]),
        /// but the async seam shares the transport behind `&self`, so it is held
        /// behind a `Mutex` — the synchronization belongs here, at the sharing
        /// point, not in the `ClawHttp` trait.
        pub fn new(inner: T) -> Self {
            Self(Mutex::new(inner))
        }
    }

    impl<T: ClawHttp + Send> ClawHttpAsync for BlockingClawHttpAsync<T> {
        fn transfer<'a>(&'a self, request: &'a HttpJsonRequest<'a>) -> HttpResponseFuture<'a> {
            // A blocking call cannot be interrupted mid-flight, so no abort flag
            // is threaded in; the async seam's `post_json` race cancels at the
            // (single) poll boundary instead.
            let never = AtomicBool::new(false);
            let result = self.0.lock().unwrap().post_json(request, &never);
            Box::pin(core::future::ready(result))
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
        inner: Mutex<T>,
        yields: u32,
    }

    impl<T> YieldingClawHttpAsync<T> {
        /// Wrap `inner`, yielding to the executor `yields` times before
        /// resolving. `yields == 0` behaves like [`BlockingClawHttpAsync`].
        ///
        /// The inner transport is driven through `&mut self`, but the async seam
        /// shares it behind `&self`, so it lives behind a `Mutex` (synchronization
        /// at the sharing point, not in the `ClawHttp` trait).
        pub fn new(inner: T, yields: u32) -> Self {
            Self {
                inner: Mutex::new(inner),
                yields,
            }
        }

        /// Borrow the wrapped blocking transport. Test-only: the in-crate tests
        /// inspect the wrapped double's call counter.
        #[cfg(test)]
        pub(crate) fn inner(&self) -> std::sync::MutexGuard<'_, T> {
            self.inner.lock().unwrap()
        }
    }

    impl<T: ClawHttp + Send> ClawHttpAsync for YieldingClawHttpAsync<T> {
        fn transfer<'a>(&'a self, request: &'a HttpJsonRequest<'a>) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                for _ in 0..self.yields {
                    yield_once().await;
                }
                // The final transfer step is the blocking inner call; the async
                // seam's `post_json` handles cancellation by dropping us.
                let never = AtomicBool::new(false);
                self.inner.lock().unwrap().post_json(request, &never)
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

    use super::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};

    /// Host [`ClawHttp`] backed by a blocking `reqwest` client.
    ///
    /// The single real transport for host CLIs and live/integration tests:
    /// honours `request.auth_type` (`"api-key"` / `"none"` / bearer), forwards
    /// extra headers, polls `abort` before sending, and treats any 2xx as success.
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

            if let Some(api_key) = request.api_key.filter(|value| !value.is_empty()) {
                match request.auth_type.unwrap_or("bearer") {
                    "api-key" => builder = builder.header("api-key", api_key),
                    "none" => {}
                    _ => builder = builder.header("Authorization", format!("Bearer {api_key}")),
                }
            }
            for header in request.headers {
                builder = builder.header(header.name, header.value);
            }

            let response = builder
                .send()
                .map_err(|error| HttpError::RequestFailed(error.to_string()))?;
            let status_code = i32::from(response.status().as_u16());
            let body = response
                .text()
                .map_err(|error| HttpError::RequestFailed(error.to_string()))?;

            if !(200..300).contains(&status_code) {
                return Err(HttpError::UnexpectedStatus(format!(
                    "HTTP {status_code}: {body}"
                )));
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

    use super::{ClawHttpAsync, HttpError, HttpJsonRequest, HttpResponse, HttpResponseFuture};

    /// Host [`ClawHttpAsync`] backed by an **async** `reqwest::Client`.
    ///
    /// The native non-blocking counterpart of the blocking `RealHttp`: it
    /// issues a genuinely concurrent request instead of blocking the polling
    /// task, so multiple in-flight calls progress together. It honours
    /// `request.auth_type` (`"api-key"` / `"none"` / bearer), forwards extra
    /// headers, and treats any 2xx as success. Cancellation is handled by the
    /// [`ClawHttpAsync::post_json`] race (dropping this future), not in `transfer`.
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
        fn transfer<'a>(&'a self, request: &'a HttpJsonRequest<'a>) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                let mut builder = self
                    .client
                    .post(request.url)
                    .header("Content-Type", "application/json")
                    .timeout(Duration::from_millis(u64::from(request.timeout_ms)))
                    .body(request.body.to_string());

                if let Some(user_agent) = &self.user_agent {
                    builder = builder.header("User-Agent", user_agent);
                }

                if let Some(api_key) = request.api_key.filter(|value| !value.is_empty()) {
                    match request.auth_type.unwrap_or("bearer") {
                        "api-key" => builder = builder.header("api-key", api_key),
                        "none" => {}
                        _ => builder = builder.header("Authorization", format!("Bearer {api_key}")),
                    }
                }
                for header in request.headers {
                    builder = builder.header(header.name, header.value);
                }

                let response = builder
                    .send()
                    .await
                    .map_err(|error| HttpError::RequestFailed(error.to_string()))?;
                let status_code = i32::from(response.status().as_u16());
                let body = response
                    .text()
                    .await
                    .map_err(|error| HttpError::RequestFailed(error.to_string()))?;

                if !(200..300).contains(&status_code) {
                    return Err(HttpError::UnexpectedStatus(format!(
                        "HTTP {status_code}: {body}"
                    )));
                }
                Ok(HttpResponse { status_code, body })
            })
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
    use super::{ClawHttp, HttpError, HttpJsonRequest, HttpResponse};
    use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

    /// `ClawHttp` that echoes the request body back with a fixed status code and
    /// counts how many times the (blocking) transport was actually invoked.
    pub struct EchoStatus {
        pub status: i32,
        pub calls: AtomicU32,
    }

    impl EchoStatus {
        pub fn new(status: i32) -> Self {
            Self {
                status,
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
            Err(HttpError::RequestFailed("simulated failure".into()))
        }
    }

    pub fn request<'a>(url: &'a str, body: &'a str) -> HttpJsonRequest<'a> {
        HttpJsonRequest {
            url,
            body,
            api_key: None,
            auth_type: None,
            timeout_ms: 1_000,
            headers: &[],
        }
    }
}

#[cfg(all(test, feature = "httpmock"))]
mod async_seam_tests {
    use super::async_test_support::{request, EchoStatus, FailingStatus};
    use super::{BlockingClawHttpAsync, Cancel, ClawHttpAsync, HttpError, YieldingClawHttpAsync};
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
        let transport = BlockingClawHttpAsync::new(EchoStatus::new(200));
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "ping");
        let response = block_on(transport.post_json(&request, Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 200);
        assert_eq!(response.body, "ping");
    }

    #[test]
    fn blocking_adapter_is_object_safe() {
        // The async seam must be shareable as a trait object, mirroring the
        // blocking `Arc<dyn ClawHttp>` usage in `claw-api`.
        let transport: Arc<dyn ClawHttpAsync> =
            Arc::new(BlockingClawHttpAsync::new(EchoStatus::new(204)));
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "{}");
        let response = block_on(transport.post_json(&request, Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 204);
    }

    #[test]
    fn blocking_adapter_resolves_in_a_single_poll() {
        let transport = BlockingClawHttpAsync::new(EchoStatus::new(200));
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
        let transport = YieldingClawHttpAsync::new(inner, 3);
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "payload");
        let (response, polls) =
            block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
        let response = response.expect("ok");
        assert_eq!(response.status_code, 200);
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
        let transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 0);
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
        let transport = YieldingClawHttpAsync::new(FailingStatus, 2);
        let abort = AtomicBool::new(false);
        let request = request("https://example.test", "{}");
        let error = block_on(transport.post_json(&request, Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)));
    }

    #[test]
    fn yielding_adapter_honors_abort() {
        let transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 2);
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
        BlockingClawHttpAsync, Cancel, ClawHttpAsync, HttpError, HttpResponse,
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
        transport: T,
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
        assert_eq!(response.status_code, 200);
        assert_eq!(response.body, "hello");
    }

    #[test]
    fn executor_drives_yielding_transport_to_completion() {
        // 5 cooperative yields means the executor must poll, observe
        // `Poll::Pending`, honor the re-armed waker, and re-poll five times
        // before the request resolves — exactly the on-device EAGAIN loop.
        let transport = YieldingClawHttpAsync::new(EchoStatus::new(200), 5);
        let response = run_async(transport, "hello", AtomicBool::new(false)).expect("ok");
        assert_eq!(response.status_code, 200);
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
        let captures: Rc<RefCell<Vec<(u32, i32)>>> = Rc::new(RefCell::new(Vec::new()));

        let mut executor: TestExecutor = AllocExecutor::new();
        for (id, yields, status) in [(1_u32, 4_u32, 201_i32), (2, 1, 202)] {
            let sink = Rc::clone(&captures);
            executor.spawn(async move {
                let transport = YieldingClawHttpAsync::new(EchoStatus::new(status), yields);
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
        assert_eq!(results, vec![(1, 201), (2, 202)]);
    }
}

/// End-to-end tests for the native async host backend [`RealHttpAsync`], driven
/// by a real **tokio** runtime against a one-shot loopback HTTP server. This
/// exercises the actual `reqwest` async client + tokio reactor — the host's
/// genuinely non-blocking transport — rather than a blocking bridge.
#[cfg(all(test, feature = "realhttp"))]
mod realhttp_async_tests {
    use super::{Cancel, ClawHttpAsync, HttpError, HttpHeader, HttpJsonRequest, RealHttpAsync};
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
            api_key: None,
            auth_type: None,
            timeout_ms: 5_000,
            headers: &[],
        }
    }

    #[test]
    fn async_reqwest_roundtrip_sends_auth_and_parses_body() {
        let (url, rx, handle) = oneshot_server("200 OK", r#"{"choices":[]}"#);
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let headers = [HttpHeader {
            name: "X-Trace",
            value: "abc",
        }];
        let request = HttpJsonRequest {
            url: &url,
            body: r#"{"model":"x"}"#,
            api_key: Some("sk-test"),
            auth_type: None, // defaults to bearer
            timeout_ms: 5_000,
            headers: &headers,
        };

        let response = block_on(http.post_json(&request, Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 200);
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
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 204);
        assert_eq!(response.body, "");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_status_299_upper_edge_is_success() {
        let (url, _rx, handle) = oneshot_server("299 Almost", "edge");
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 299);
        assert_eq!(response.body, "edge");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_status_300_just_outside_is_unexpected() {
        // 300 is the exclusive upper bound of `200..300`; no `Location`, so the
        // default redirect policy does not follow it.
        let (url, _rx, handle) = oneshot_server("300 Multiple Choices", "nope");
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
        match error {
            HttpError::UnexpectedStatus(message) => {
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
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
        match error {
            HttpError::UnexpectedStatus(message) => {
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
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            api_key: Some("secret"),
            auth_type: Some("api-key"),
            timeout_ms: 5_000,
            headers: &[],
        };
        block_on(http.post_json(&request, Cancel::new(&abort))).expect("ok");
        let raw = rx.recv().expect("captured");
        assert!(raw.contains("api-key: secret"), "raw: {raw}");
        assert!(!raw.to_lowercase().contains("authorization:"), "raw: {raw}");
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_auth_none_omits_authorization_even_with_key() {
        let (url, rx, handle) = oneshot_server("200 OK", "{}");
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            api_key: Some("secret"),
            auth_type: Some("none"),
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
    fn async_reqwest_empty_api_key_omits_authorization() {
        let (url, rx, handle) = oneshot_server("200 OK", "{}");
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            api_key: Some(""), // empty key: no auth header at all
            auth_type: None,
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
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 200);
        assert!(response.body.is_empty());
        handle.join().expect("server thread");
    }

    #[test]
    fn async_reqwest_large_body_roundtrip() {
        // Larger than the server's request read buffer and any single TCP read,
        // so the client must accumulate the full Content-Length.
        let big = "a".repeat(64 * 1024);
        let (url, _rx, handle) = oneshot_server("200 OK", big.clone());
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
        assert_eq!(response.status_code, 200);
        assert_eq!(response.body.len(), big.len());
        assert_eq!(response.body, big);
        handle.join().expect("server thread");
    }

    // ---- failure modes ------------------------------------------------------

    #[test]
    fn async_reqwest_honors_abort_before_send() {
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(true);
        let error =
            block_on(http.post_json(&req("http://127.0.0.1:9/never", "{}"), Cancel::new(&abort)))
                .unwrap_err();
        assert!(matches!(error, HttpError::Aborted));
    }

    #[test]
    fn async_reqwest_connection_refused_is_request_failed() {
        let url = refused_url();
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
    }

    #[test]
    fn async_reqwest_timeout_is_request_failed() {
        let url = stalling_server(2_000);
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let request = HttpJsonRequest {
            url: &url,
            body: "{}",
            api_key: None,
            auth_type: None,
            timeout_ms: 150, // fires well before the server's 2s hold
            headers: &[],
        };
        let error = block_on(http.post_json(&request, Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
    }

    #[test]
    fn async_reqwest_invalid_url_is_request_failed() {
        let http = RealHttpAsync::new();
        let abort = AtomicBool::new(false);
        let error =
            block_on(http.post_json(&req("not a url", "{}"), Cancel::new(&abort))).unwrap_err();
        assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
    }
}
