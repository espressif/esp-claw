//! `claw_sys` — the thin shims to C/IDF facilities that Rust `std` cannot
//! express on its own: the `ESP_LOGx` log sink (the C↔Rust logging bridge) and
//! the `esp_http_client` networking driver.
//!
//! The upper-layer logging that drives [`log_sink`] — the `log` facade backend
//! and the flat-tree `tracing` subscriber — lives in the `claw-log` crate.

pub mod executor;
pub mod fs;
pub mod http;
pub mod log_sink;
pub mod thread;
pub mod timer;

#[cfg(target_os = "espidf")]
pub use executor::EspIdfExecutor;
#[cfg(target_os = "espidf")]
pub use fs::{EspIdfFile, EspIdfFs};
#[cfg(target_os = "espidf")]
pub use http::EspIdfHttp;
#[cfg(target_os = "espidf")]
pub use thread::EspIdfThread;
#[cfg(target_os = "espidf")]
pub use timer::EspIdfTimer;
