//! `claw_sys` — the thin shims to C/IDF facilities that Rust `std` cannot
//! express on its own: the `ESP_LOGx` log sink (the C↔Rust logging bridge) and
//! the `esp_http_client` networking driver.
//!
//! The upper-layer logging that drives [`log_sink`] — the `log` facade backend
//! and the flat-tree `tracing` subscriber — lives in the `claw-log` crate.

pub mod fs;
pub mod http;
pub mod log_sink;
pub mod thread;
pub mod timer;

#[cfg(target_os = "espidf")]
pub use fs::{EspIdfFile, EspIdfFs};
#[cfg(target_os = "espidf")]
pub use http::{EspIdfHttp, EspIdfHttpOneShot};
#[cfg(target_os = "espidf")]
pub use thread::EspIdfThread;
#[cfg(target_os = "espidf")]
pub use timer::EspIdfTimer;

#[cfg(test)]
mod tests {
    use super::http::{build_auth_header, parse_error_message_body, truncate};
    use claw_interface::http::{HttpAuth, HttpStatusCode};

    #[test]
    fn auth_header_bearer_default() {
        let (name, value) = build_auth_header(HttpAuth::Bearer("sk-123")).unwrap();
        assert_eq!(name, "Authorization");
        assert_eq!(value, "Bearer sk-123");
    }

    #[test]
    fn auth_header_bearer_explicit() {
        let (name, value) = build_auth_header(HttpAuth::Bearer("sk-123")).unwrap();
        assert_eq!(name, "Authorization");
        assert_eq!(value, "Bearer sk-123");
    }

    #[test]
    fn auth_header_api_key() {
        let (name, value) = build_auth_header(HttpAuth::ApiKey("sk-123")).unwrap();
        assert_eq!(name, "X-API-Key");
        assert_eq!(value, "sk-123");
    }

    #[test]
    fn auth_header_none_or_empty() {
        assert!(build_auth_header(HttpAuth::None).is_none());
        assert!(build_auth_header(HttpAuth::Bearer("")).is_none());
        assert!(build_auth_header(HttpAuth::ApiKey("")).is_none());
    }

    #[test]
    fn error_body_prefers_error_message() {
        let body = r#"{"error":{"message":"bad key"}}"#;
        assert_eq!(
            parse_error_message_body(body, HttpStatusCode::new(401)),
            "HTTP 401: bad key"
        );
    }

    #[test]
    fn error_body_top_level_message() {
        let body = r#"{"message":"rate limited"}"#;
        assert_eq!(
            parse_error_message_body(body, HttpStatusCode::new(429)),
            "HTTP 429: rate limited"
        );
    }

    #[test]
    fn error_body_non_json_truncates() {
        assert_eq!(
            parse_error_message_body("oops", HttpStatusCode::new(500)),
            "HTTP 500: oops"
        );
        assert_eq!(
            parse_error_message_body("", HttpStatusCode::new(500)),
            "HTTP 500"
        );
    }

    #[test]
    fn error_body_falls_back_when_message_not_a_string() {
        // A non-string `error.message` is ignored in favor of the top-level one.
        let body = r#"{"error":{"message":123},"message":"fallback"}"#;
        assert_eq!(
            parse_error_message_body(body, HttpStatusCode::new(400)),
            "HTTP 400: fallback"
        );
    }

    #[test]
    fn truncate_keeps_short_strings() {
        assert_eq!(truncate("hello", 160), "hello");
    }

    #[test]
    fn truncate_backs_off_to_char_boundary() {
        // "é" is two bytes; a cut at byte 3 must back off so it never splits a char.
        assert_eq!(truncate("ééé", 3), "é");
    }
}
