//! ESP-IDF executor: the device-side [`ClawExecutor`].
//!
//! Drives the orchestrator's `!Send` engine future on the device worker via
//! `edge-executor`'s cooperative `LocalExecutor` + `block_on`. No tokio: the esp
//! HTTP (`EspIdfHttp`) and timer (`EspIdfTimer`) seams are real-waker futures
//! that self-wake under a bare `block_on`, so a reactor is unnecessary.

#[cfg(target_os = "espidf")]
use claw_interface::ClawExecutor;
#[cfg(target_os = "espidf")]
use core::future::Future;

/// Device [`ClawExecutor`] backed by `edge-executor`.
#[cfg(target_os = "espidf")]
#[derive(Debug, Default, Clone, Copy)]
pub struct EspIdfExecutor;

#[cfg(target_os = "espidf")]
impl ClawExecutor for EspIdfExecutor {
    fn block_on<Fut: Future>(future: Fut) -> Fut::Output {
        let executor = edge_executor::LocalExecutor::<4>::new();
        futures_lite::future::block_on(executor.run(future))
    }
}
