//! Per-call retry loop used by [`crate::ClawApi`].
//!
//! Retry is a per-request policy ([`crate::RetryPolicy`] on each request),
//! so the loop lives just above the backend call rather than in the transport.
//! Only operations whose error reports [`is_retryable`](crate::ClawApiError::is_retryable)
//! are retried; the backoff sleep polls the abort flag cooperatively.

use core::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use crate::types::RetryPolicy;

/// Abort poll granularity while sleeping for backoff.
const BACKOFF_POLL_SLICE_MS: u64 = 25;

/// Run `op`, retrying transient failures per `policy`.
///
/// `is_retryable` classifies an error; `on_abort` produces the error returned
/// when the abort flag fires during a backoff sleep.
pub fn run_with_retry<T, E>(
    policy: &RetryPolicy,
    abort: &AtomicBool,
    is_retryable: impl Fn(&E) -> bool,
    on_abort: impl Fn() -> E,
    mut op: impl FnMut() -> Result<T, E>,
) -> Result<T, E> {
    let mut attempt = 0u32;
    loop {
        match op() {
            Ok(value) => return Ok(value),
            Err(err) => {
                if !is_retryable(&err) || attempt >= policy.max_retries {
                    return Err(err);
                }
                attempt += 1;
                if !sleep_abortable(policy.backoff_ms(attempt), abort) {
                    return Err(on_abort());
                }
            }
        }
    }
}

/// Sleep `total_ms`, polling `abort`. Returns `false` if aborted.
fn sleep_abortable(total_ms: u32, abort: &AtomicBool) -> bool {
    let mut remaining = total_ms as u64;
    while remaining > 0 {
        if abort.load(Ordering::Acquire) {
            return false;
        }
        let slice = remaining.min(BACKOFF_POLL_SLICE_MS);
        std::thread::sleep(Duration::from_millis(slice));
        remaining -= slice;
    }
    !abort.load(Ordering::Acquire)
}
