//! Runtime-agnostic async timer seam.
//!
//! `claw-api` needs retry backoff in async code, but it must not depend on a
//! specific executor such as tokio, edge-executor, or embedded-executor. This
//! trait is the injected boundary: each runtime supplies a small wrapper that
//! waits using that runtime's timer primitive.

use core::future::Future;
use core::pin::Pin;
use core::time::Duration;

use crate::http::Cancel;

/// Boxed future returned by [`ClawTimer::sleep`].
///
/// The future borrows the timer implementation and the caller-owned
/// cancellation token, so it cannot outlive either one.
pub type TimerFuture<'a> = Pin<Box<dyn Future<Output = SleepOutcome> + 'a>>;

/// Result of an abortable sleep.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SleepOutcome {
    /// The requested duration elapsed.
    Completed,
    /// Cancellation was observed before the duration elapsed.
    Cancelled,
}

impl SleepOutcome {
    /// Whether the sleep completed normally.
    pub const fn is_completed(self) -> bool {
        matches!(self, Self::Completed)
    }

    /// Whether cancellation interrupted the sleep.
    pub const fn is_cancelled(self) -> bool {
        matches!(self, Self::Cancelled)
    }
}

/// Async timer injection point.
///
/// Thread-safety is intentionally not a supertrait requirement. A host wrapper
/// around tokio may be freely shareable, while an embedded timer may be
/// task-local. Require `Send`/`Sync` only at the caller boundary that actually
/// crosses threads.
///
/// Implementations should check [`Cancel`] before sleeping and at their natural
/// timer granularity while sleeping. For runtimes whose sleep future cannot be
/// externally woken by an atomic flag, implement this by sleeping in short
/// slices and checking `cancel` between slices.
pub trait ClawTimer {
    /// Sleep for `duration`, returning [`SleepOutcome::Cancelled`] if
    /// cancellation is observed first.
    fn sleep<'a>(&'a mut self, duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a>;
}

#[cfg(feature = "tokiotimer")]
pub mod tokio_timer {
    use super::{Cancel, ClawTimer, Duration, SleepOutcome, TimerFuture};

    const CANCEL_POLL_INTERVAL: Duration = Duration::from_millis(50);

    /// Host timer backed by `tokio::time::sleep`.
    ///
    /// Cancellation is an atomic flag rather than a waker-aware token, so long
    /// sleeps are split into short slices and the flag is checked between them.
    #[derive(Debug, Default, Clone, Copy)]
    pub struct TokioTimer;

    impl ClawTimer for TokioTimer {
        fn sleep<'a>(&'a mut self, duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
            Box::pin(async move {
                if cancel.is_cancelled() {
                    return SleepOutcome::Cancelled;
                }

                let mut remaining = duration;
                while remaining > Duration::ZERO {
                    let slice = remaining.min(CANCEL_POLL_INTERVAL);
                    tokio::time::sleep(slice).await;
                    if cancel.is_cancelled() {
                        return SleepOutcome::Cancelled;
                    }
                    remaining = remaining.saturating_sub(slice);
                }

                SleepOutcome::Completed
            })
        }
    }
}

#[cfg(any(test, feature = "timermock"))]
pub mod mock {
    use super::{Cancel, ClawTimer, Duration, SleepOutcome, TimerFuture};
    use core::future::Future;
    use core::pin::Pin;
    use core::task::{Context, Poll};

    /// A test timer that completes immediately without waiting.
    #[derive(Debug, Default, Clone, Copy)]
    pub struct ImmediateTimer;

    impl ClawTimer for ImmediateTimer {
        fn sleep<'a>(&'a mut self, _duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
            Box::pin(async move {
                if cancel.is_cancelled() {
                    SleepOutcome::Cancelled
                } else {
                    SleepOutcome::Completed
                }
            })
        }
    }

    /// A test timer that yields a fixed number of polls before completing.
    ///
    /// This models a cooperative executor shape without depending on wall-clock
    /// time. It is not a production timer.
    #[derive(Debug, Clone, Copy)]
    pub struct YieldingTimer {
        yields: u32,
    }

    impl YieldingTimer {
        /// Create a timer that returns `Poll::Pending` `yields` times before
        /// completing.
        pub const fn new(yields: u32) -> Self {
            Self { yields }
        }
    }

    impl ClawTimer for YieldingTimer {
        fn sleep<'a>(&'a mut self, _duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
            let yields = self.yields;
            Box::pin(async move {
                for _ in 0..yields {
                    if cancel.is_cancelled() {
                        return SleepOutcome::Cancelled;
                    }
                    yield_once().await;
                }
                if cancel.is_cancelled() {
                    SleepOutcome::Cancelled
                } else {
                    SleepOutcome::Completed
                }
            })
        }
    }

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

        YieldOnce(false).await;
    }
}

#[cfg(test)]
mod tests {
    use super::mock::{ImmediateTimer, YieldingTimer};
    use super::{ClawTimer, SleepOutcome};
    use crate::Cancel;
    use core::future::Future;
    use core::sync::atomic::{AtomicBool, Ordering};
    use core::task::{Context, Poll};
    use std::sync::Arc;
    use std::task::{Wake, Waker};
    use std::time::Duration;

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on_counting<F: Future>(future: F) -> (F::Output, u32) {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        let mut polls = 0;
        loop {
            polls += 1;
            if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
                return (output, polls);
            }
        }
    }

    #[test]
    fn immediate_timer_completes_without_waiting() {
        let mut timer = ImmediateTimer;
        let abort = AtomicBool::new(false);
        let outcome =
            block_on_counting(timer.sleep(Duration::from_millis(500), Cancel::new(&abort))).0;

        assert_eq!(outcome, SleepOutcome::Completed);
        assert!(outcome.is_completed());
    }

    #[test]
    fn timer_reports_pre_cancelled_token() {
        let mut timer = ImmediateTimer;
        let abort = AtomicBool::new(true);
        let outcome =
            block_on_counting(timer.sleep(Duration::from_millis(500), Cancel::new(&abort))).0;

        assert_eq!(outcome, SleepOutcome::Cancelled);
        assert!(outcome.is_cancelled());
    }

    #[test]
    fn yielding_timer_yields_before_completing() {
        let mut timer = YieldingTimer::new(3);
        let abort = AtomicBool::new(false);
        let (outcome, polls) =
            block_on_counting(timer.sleep(Duration::from_millis(500), Cancel::new(&abort)));

        assert_eq!(outcome, SleepOutcome::Completed);
        assert_eq!(polls, 4);
    }

    #[test]
    fn yielding_timer_observes_cancellation_between_yields() {
        let mut timer = YieldingTimer::new(3);
        let abort = AtomicBool::new(false);
        let future = async {
            abort.store(true, Ordering::Relaxed);
            timer
                .sleep(Duration::from_millis(500), Cancel::new(&abort))
                .await
        };
        let outcome = block_on_counting(future).0;

        assert_eq!(outcome, SleepOutcome::Cancelled);
    }
}
