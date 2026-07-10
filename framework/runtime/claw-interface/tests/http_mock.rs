#![cfg(feature = "httpmock")]

use core::future::Future;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::task::{Context, Poll, Waker};

use claw_interface::http::blocking;
use claw_interface::{
    BlockingHttpAdapter, Cancel, ClawHttp, HttpAuth, HttpError, HttpJsonRequest,
    HttpRequestFailure, HttpResponse, HttpStatusCode, YieldingHttpAdapter,
};

#[test]
fn blocking_adapter_drives_clawhttp_through_async_seam() {
    let mut transport = BlockingHttpAdapter::new(EchoStatus::new(200));
    let abort = AtomicBool::new(false);
    let request = request("https://example.test", "ping");
    let response = block_on(transport.post_json(&request, Cancel::new(&abort))).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::OK);
    assert_eq!(response.body, "ping");
}

#[test]
fn blocking_adapter_is_object_safe() {
    let mut transport: Box<dyn ClawHttp> = Box::new(BlockingHttpAdapter::new(EchoStatus::new(204)));
    let abort = AtomicBool::new(false);
    let request = request("https://example.test", "{}");
    let response = block_on(transport.post_json(&request, Cancel::new(&abort))).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::NO_CONTENT);
}

#[test]
fn blocking_adapter_resolves_in_a_single_poll() {
    let mut transport = BlockingHttpAdapter::new(EchoStatus::new(200));
    let abort = AtomicBool::new(false);
    let request = request("https://example.test", "{}");
    let (response, polls) = block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
    assert!(response.is_ok());
    assert_eq!(polls, 1);
}

#[test]
fn yielding_adapter_yields_before_resolving() {
    let mut transport = YieldingHttpAdapter::new(EchoStatus::new(200), 3);
    let abort = AtomicBool::new(false);
    let request = request("https://example.test", "payload");
    let (response, polls) = block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
    let response = response.expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::OK);
    assert_eq!(response.body, "payload");
    assert_eq!(polls, 4);
}

#[test]
fn yielding_adapter_zero_yields_resolves_in_one_poll() {
    let mut transport = YieldingHttpAdapter::new(EchoStatus::new(200), 0);
    let abort = AtomicBool::new(false);
    let request = request("https://example.test", "edge");
    let (response, polls) = block_on_counting(transport.post_json(&request, Cancel::new(&abort)));
    let response = response.expect("ok");
    assert_eq!(response.body, "edge");
    assert_eq!(polls, 1);
}

#[test]
fn yielding_adapter_propagates_transport_error() {
    let mut transport = YieldingHttpAdapter::new(FailingStatus, 2);
    let abort = AtomicBool::new(false);
    let request = request("https://example.test", "{}");
    let error = block_on(transport.post_json(&request, Cancel::new(&abort))).unwrap_err();
    assert!(matches!(error, HttpError::RequestFailed(_)));
}

#[test]
fn yielding_adapter_honors_abort() {
    let mut transport = YieldingHttpAdapter::new(EchoStatus::new(200), 2);
    let abort = AtomicBool::new(true);
    let request = request("https://example.test", "{}");
    let error = block_on(transport.post_json(&request, Cancel::new(&abort))).unwrap_err();
    assert!(matches!(error, HttpError::Aborted));
}

struct EchoStatus {
    status: HttpStatusCode,
    calls: AtomicU32,
}

impl EchoStatus {
    fn new(status: u16) -> Self {
        Self {
            status: HttpStatusCode::new(status),
            calls: AtomicU32::new(0),
        }
    }
}

impl blocking::ClawHttp for EchoStatus {
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

struct FailingStatus;

impl blocking::ClawHttp for FailingStatus {
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

fn request<'a>(url: &'a str, body: &'a str) -> HttpJsonRequest<'a> {
    HttpJsonRequest {
        url,
        body,
        auth: HttpAuth::None,
        timeout_ms: 1_000,
        headers: &[],
    }
}

fn block_on<F: Future>(future: F) -> F::Output {
    block_on_counting(future).0
}

fn block_on_counting<F: Future>(future: F) -> (F::Output, u32) {
    let mut future = Box::pin(future);
    let mut context = Context::from_waker(Waker::noop());
    let mut polls = 0_u32;
    loop {
        polls += 1;
        if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
            return (output, polls);
        }
    }
}
