//! Runtime-agnostic executor seam.
//!
//! The orchestrator drives its `!Send` engine future to completion on one owned
//! worker thread. *How* that `block_on` works is runtime-specific: the device
//! uses a cooperative `edge-executor` `block_on` (no reactor; the esp HTTP/timer
//! seams self-wake), while the host needs a tokio runtime so async `reqwest` and
//! `TokioTimer` — which poll against tokio's reactor — make progress. This trait
//! is that injection point, so `claw-core` stays executor-agnostic (it depends
//! only on the trait, never on `edge-executor` or `tokio`).

use core::future::Future;

/// Injection point for "run this future to completion on the current thread".
///
/// The engine future is `!Send` and self-driving — it multiplexes every session
/// via its own poll loop and never spawns onto the executor — so an
/// implementation only needs a `block_on`, not a task spawner. The method is
/// generic, so this is a static-dispatch seam (never used as `dyn`), like
/// [`crate::ClawThread`].
pub trait ClawExecutor {
    /// Drive `future` (which may be `!Send`) to completion, returning its output.
    fn block_on<Fut: Future>(future: Fut) -> Fut::Output;
}

#[cfg(feature = "tokioexecutor")]
pub use tokio_executor::TokioExecutor;

#[cfg(feature = "tokioexecutor")]
mod tokio_executor {
    use super::{ClawExecutor, Future};

    /// Host executor backed by a current-thread tokio runtime.
    ///
    /// The orchestrator's worker calls `block_on` exactly once and stays parked in
    /// it for the whole session lifetime, so building a runtime here is not a hot
    /// path. `enable_all` turns on the time + IO drivers that async `reqwest` and
    /// `TokioTimer` poll against. A current-thread runtime's `block_on` accepts the
    /// `!Send` engine future.
    #[derive(Debug, Default, Clone, Copy)]
    pub struct TokioExecutor;

    impl ClawExecutor for TokioExecutor {
        fn block_on<Fut: Future>(future: Fut) -> Fut::Output {
            let runtime = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .expect("build current-thread tokio runtime for the orchestrator engine");
            runtime.block_on(future)
        }
    }
}
