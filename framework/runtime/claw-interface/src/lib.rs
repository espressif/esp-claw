//! `claw_interface` — the OS / platform abstraction layer for the claw Rust
//! crates.
//!
//! This is the inbound boundary (C / OS -> Rust): it defines the
//! dependency-injection traits that abstract over platform facilities —
//! filesystem ([`ClawFs`](fs::ClawFs)) and networking ([`ClawHttp`](http::ClawHttp)) —
//! plus the shared types those traits work with. The pure-Rust core crates
//! (`claw-api`, `claw_core`, `claw-capability`, `claw-memory`, `claw-sandbox`, …)
//! depend only on these traits, never on a platform directly, so the device
//! build and host tests can plug in different implementations of the same seam.

pub mod fs;
pub mod http;
pub mod thread;

pub use fs::{ClawFile, ClawFs, FsError};
#[cfg(feature = "diskfs")]
pub use fs::{DiskFile, DiskFs};
#[cfg(feature = "memfs")]
pub use fs::{MemFile, MemFs};
#[cfg(feature = "httpmock")]
pub use http::{
    BlockingClawHttpAsync, CapturingHttp, FailingHttp, NeverHttp, NoopHttp, ScriptStep,
    ScriptedHttp, SharedScriptHttp, YieldingClawHttpAsync,
};
pub use http::{
    Cancel, ClawHttp, ClawHttpAsync, HttpError, HttpHeader, HttpJsonRequest, HttpResponse,
    HttpResponseFuture,
};
#[cfg(feature = "realhttp")]
pub use http::{RealHttp, RealHttpAsync};
#[cfg(feature = "stdthread")]
pub use thread::StdThread;
pub use thread::{ClawThread, CoreAffinity, Priority, WorkerHandle};
