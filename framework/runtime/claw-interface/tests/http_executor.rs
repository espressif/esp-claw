#![cfg(feature = "httpmock")]

use core::sync::atomic::AtomicBool;
use std::cell::RefCell;
use std::rc::Rc;

use claw_interface::{
    BlockingHttpAdapter, Cancel, ClawHttp, HttpAuth, HttpError, HttpJsonRequest, HttpResponse,
    HttpStatusCode, YieldingHttpAdapter,
};
use embedded_executor::{AllocExecutor, Sleep, Wake};
use lock_api::{GuardSend, RawMutex};

struct RawSpinlock(AtomicBool);

// SAFETY: `lock` acquires the atomic flag with Acquire ordering and `unlock`
// releases it with Release ordering. These tests are single-threaded, but this
// still satisfies `RawMutex`'s critical-section contract.
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
            .compare_exchange(
                false,
                true,
                core::sync::atomic::Ordering::Acquire,
                core::sync::atomic::Ordering::Relaxed,
            )
            .is_ok()
    }

    unsafe fn unlock(&self) {
        self.0.store(false, core::sync::atomic::Ordering::Release);
    }
}

#[derive(Clone, Copy, Default)]
struct NopSleep;

impl Sleep for NopSleep {
    fn sleep(&self) {}
}

impl Wake for NopSleep {
    fn wake(&self) {}
}

type TestExecutor<'a> = AllocExecutor<'a, RawSpinlock, NopSleep>;

#[test]
fn executor_drives_blocking_transport_to_completion() {
    let transport = BlockingHttpAdapter::new(EchoStatus::new(200));
    let response = run_async(transport, "hello", AtomicBool::new(false)).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::OK);
    assert_eq!(response.body, "hello");
}

#[test]
fn executor_drives_yielding_transport_to_completion() {
    let transport = YieldingHttpAdapter::new(EchoStatus::new(200), 5);
    let response = run_async(transport, "hello", AtomicBool::new(false)).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::OK);
    assert_eq!(response.body, "hello");
}

#[test]
fn executor_propagates_transport_error() {
    let transport = YieldingHttpAdapter::new(FailingStatus, 3);
    let error = run_async(transport, "{}", AtomicBool::new(false)).unwrap_err();
    assert!(matches!(error, HttpError::RequestFailed(_)));
}

#[test]
fn executor_honors_abort() {
    let transport = YieldingHttpAdapter::new(EchoStatus::new(200), 2);
    let error = run_async(transport, "{}", AtomicBool::new(true)).unwrap_err();
    assert!(matches!(error, HttpError::Aborted));
}

#[test]
fn executor_interleaves_concurrent_requests() {
    let captures: Rc<RefCell<Vec<(u32, HttpStatusCode)>>> = Rc::new(RefCell::new(Vec::new()));
    let mut executor: TestExecutor = AllocExecutor::new();

    for (id, yields, status) in [(1_u32, 4_u32, 201_u16), (2, 1, 202)] {
        let sink = Rc::clone(&captures);
        executor.spawn(async move {
            let mut transport = YieldingHttpAdapter::new(EchoStatus::new(status), yields);
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

fn run_async<T>(
    mut transport: T,
    body: &'static str,
    abort: AtomicBool,
) -> Result<HttpResponse, HttpError>
where
    T: ClawHttp + 'static,
{
    let sink: Rc<RefCell<Option<Result<HttpResponse, HttpError>>>> = Rc::new(RefCell::new(None));
    let result_sink = Rc::clone(&sink);

    let mut executor: TestExecutor = AllocExecutor::new();
    executor.spawn(async move {
        let request = request("https://example.test", body);
        let result = transport.post_json(&request, Cancel::new(&abort)).await;
        *result_sink.borrow_mut() = Some(result);
    });
    executor.run();

    let result = sink
        .borrow_mut()
        .take()
        .expect("executor finished without capturing a result");
    result
}

fn request<'a>(url: &'a str, body: &'a str) -> HttpJsonRequest<'a> {
    HttpJsonRequest {
        url,
        body,
        auth: HttpAuth::None,
        timeout_ms: 1_000,
        headers: &[],
    }
}

struct EchoStatus {
    status: HttpStatusCode,
}

impl EchoStatus {
    fn new(status: u16) -> Self {
        Self {
            status: HttpStatusCode::new(status),
        }
    }
}

impl claw_interface::http::blocking::ClawHttp for EchoStatus {
    fn post_json(
        &mut self,
        request: &HttpJsonRequest,
        abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        if abort.load(core::sync::atomic::Ordering::Acquire) {
            return Err(HttpError::Aborted);
        }
        Ok(HttpResponse {
            status_code: self.status,
            body: request.body.to_string(),
        })
    }
}

struct FailingStatus;

impl claw_interface::http::blocking::ClawHttp for FailingStatus {
    fn post_json(
        &mut self,
        _request: &HttpJsonRequest,
        _abort: &AtomicBool,
    ) -> Result<HttpResponse, HttpError> {
        Err(HttpError::RequestFailed(
            claw_interface::HttpRequestFailure::transport("simulated failure"),
        ))
    }
}
