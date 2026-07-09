#![cfg(feature = "realhttp")]

use core::sync::atomic::AtomicBool;
use std::io::{Read, Write};
use std::net::TcpListener;
use std::sync::mpsc;
use std::thread::JoinHandle;
use std::time::Duration;

use claw_interface::{
    Cancel, ClawHttp, HttpAuth, HttpError, HttpHeader, HttpJsonRequest, HttpStatusCode, RealHttp,
};

#[test]
fn async_reqwest_roundtrip_sends_auth_and_parses_body() {
    let (url, rx, handle) = oneshot_server("200 OK", r#"{"choices":[]}"#);
    let mut http = RealHttp::new();
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

    let raw = rx.recv().expect("server captured request");
    assert!(raw.starts_with("POST /v1/chat/completions "), "raw: {raw}");
    assert!(raw.contains("authorization: Bearer sk-test"), "raw: {raw}");
    assert!(raw.contains("x-trace: abc"), "raw: {raw}");
    assert!(raw.contains(r#"{"model":"x"}"#), "raw: {raw}");
    handle.join().expect("server thread");
}

#[test]
fn async_reqwest_status_204_no_content_is_success() {
    let (url, _rx, handle) = oneshot_server("204 No Content", "");
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::NO_CONTENT);
    assert_eq!(response.body, "");
    handle.join().expect("server thread");
}

#[test]
fn async_reqwest_status_299_upper_edge_is_success() {
    let (url, _rx, handle) = oneshot_server("299 Almost", "edge");
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::new(299));
    assert_eq!(response.body, "edge");
    handle.join().expect("server thread");
}

#[test]
fn async_reqwest_status_300_just_outside_is_unexpected() {
    let (url, _rx, handle) = oneshot_server("300 Multiple Choices", "nope");
    let mut http = RealHttp::new();
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
    let mut http = RealHttp::new();
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

#[test]
fn async_reqwest_api_key_auth_uses_api_key_header() {
    let (url, rx, handle) = oneshot_server("200 OK", "{}");
    let mut http = RealHttp::new();
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
    let mut http = RealHttp::new();
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
    let mut http = RealHttp::new();
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

#[test]
fn async_reqwest_empty_200_body_is_ok() {
    let (url, _rx, handle) = oneshot_server("200 OK", "");
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::OK);
    assert!(response.body.is_empty());
    handle.join().expect("server thread");
}

#[test]
fn async_reqwest_large_body_roundtrip() {
    let big = "a".repeat(64 * 1024);
    let (url, _rx, handle) = oneshot_server("200 OK", big.clone());
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let response = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).expect("ok");
    assert_eq!(response.status_code, HttpStatusCode::OK);
    assert_eq!(response.body.len(), big.len());
    assert_eq!(response.body, big);
    handle.join().expect("server thread");
}

#[test]
fn async_reqwest_honors_abort_before_send() {
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(true);
    let error =
        block_on(http.post_json(&req("http://127.0.0.1:9/never", "{}"), Cancel::new(&abort)))
            .unwrap_err();
    assert!(matches!(error, HttpError::Aborted));
}

#[test]
fn async_reqwest_connection_refused_is_request_failed() {
    let url = refused_url();
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let error = block_on(http.post_json(&req(&url, "{}"), Cancel::new(&abort))).unwrap_err();
    assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
}

#[test]
fn async_reqwest_timeout_is_request_failed() {
    let url = stalling_server(2_000);
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let request = HttpJsonRequest {
        url: &url,
        body: "{}",
        auth: HttpAuth::None,
        timeout_ms: 150,
        headers: &[],
    };

    let error = block_on(http.post_json(&request, Cancel::new(&abort))).unwrap_err();
    assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
}

#[test]
fn async_reqwest_invalid_url_is_request_failed() {
    let mut http = RealHttp::new();
    let abort = AtomicBool::new(false);
    let error = block_on(http.post_json(&req("not a url", "{}"), Cancel::new(&abort))).unwrap_err();
    assert!(matches!(error, HttpError::RequestFailed(_)), "{error:?}");
}

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

fn refused_url() -> String {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
    let addr = listener.local_addr().expect("local addr");
    drop(listener);
    format!("http://{addr}/")
}

fn stalling_server(hold_ms: u64) -> String {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind loopback");
    let addr = listener.local_addr().expect("local addr");
    std::thread::spawn(move || {
        if let Ok((mut stream, _)) = listener.accept() {
            let mut buf = [0u8; 1024];
            let _ = stream.read(&mut buf);
            std::thread::sleep(Duration::from_millis(hold_ms));
        }
    });
    format!("http://{addr}/")
}

fn block_on<F: core::future::Future>(future: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("tokio runtime")
        .block_on(future)
}

fn req<'a>(url: &'a str, body: &'a str) -> HttpJsonRequest<'a> {
    HttpJsonRequest {
        url,
        body,
        auth: HttpAuth::None,
        timeout_ms: 5_000,
        headers: &[],
    }
}
