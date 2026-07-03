//! ESP-IDF timer implementation for async retry backoff.

#[cfg(target_os = "espidf")]
use claw_interface::{Cancel, ClawTimer, SleepOutcome, TimerFuture};
#[cfg(target_os = "espidf")]
use core::{future::Future, pin::Pin, task::Context, task::Poll, time::Duration};
#[cfg(target_os = "espidf")]
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex, MutexGuard,
};
#[cfg(target_os = "espidf")]
use std::task::Waker;
#[cfg(target_os = "espidf")]
use std::time::Instant;

#[cfg(target_os = "espidf")]
const CANCEL_POLL_INTERVAL: Duration = Duration::from_millis(50);

/// Device timer used by `ClawApiAsync` retry backoff.
#[cfg(target_os = "espidf")]
#[derive(Clone, Copy, Default)]
pub struct EspIdfTimer;

#[cfg(target_os = "espidf")]
impl ClawTimer for EspIdfTimer {
    fn sleep<'a>(&'a mut self, duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
        Box::pin(EspIdfSleep::new(duration, cancel))
    }
}

#[cfg(target_os = "espidf")]
struct EspIdfSleep<'cancel> {
    cancel: Cancel<'cancel>,
    deadline: Instant,
    state: Arc<SleepState>,
    started: bool,
}

#[cfg(target_os = "espidf")]
struct SleepState {
    fired: AtomicBool,
    stopped: AtomicBool,
    waker: Mutex<Option<Waker>>,
}

#[cfg(target_os = "espidf")]
impl<'cancel> EspIdfSleep<'cancel> {
    fn new(duration: Duration, cancel: Cancel<'cancel>) -> Self {
        let now = Instant::now();
        let deadline = now.checked_add(duration).unwrap_or(now);
        Self {
            cancel,
            deadline,
            state: Arc::new(SleepState {
                fired: AtomicBool::new(duration == Duration::ZERO),
                stopped: AtomicBool::new(false),
                waker: Mutex::new(None),
            }),
            started: false,
        }
    }

    /// Start the backoff thread. Returns `false` if the thread could not be
    /// spawned, so the caller can resolve the sleep as `Completed` (skip the
    /// backoff) instead of hanging — a spawn failure is surfaced, never fatal.
    fn start(&mut self) -> bool {
        if self.started {
            return true;
        }
        self.started = true;

        let state = Arc::clone(&self.state);
        let deadline = self.deadline;
        let spawn_result = std::thread::Builder::new()
            .name("claw_timer".to_string())
            .spawn(move || timer_thread(deadline, state));
        if spawn_result.is_err() {
            log::warn!("claw_timer thread spawn failed; skipping backoff sleep");
            return false;
        }
        true
    }
}

#[cfg(target_os = "espidf")]
impl Future for EspIdfSleep<'_> {
    type Output = SleepOutcome;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        if self.cancel.is_cancelled() {
            self.state.stopped.store(true, Ordering::Release);
            return Poll::Ready(SleepOutcome::Cancelled);
        }

        if self.state.fired.load(Ordering::Acquire) || Instant::now() >= self.deadline {
            return Poll::Ready(SleepOutcome::Completed);
        }

        *lock(&self.state.waker) = Some(context.waker().clone());
        if !self.start() {
            // Could not spawn the backoff thread; resolve as completed so the
            // retry proceeds without waiting rather than stalling forever.
            return Poll::Ready(SleepOutcome::Completed);
        }
        Poll::Pending
    }
}

#[cfg(target_os = "espidf")]
impl Drop for EspIdfSleep<'_> {
    fn drop(&mut self) {
        self.state.stopped.store(true, Ordering::Release);
    }
}

#[cfg(target_os = "espidf")]
fn timer_thread(deadline: Instant, state: Arc<SleepState>) {
    loop {
        if state.stopped.load(Ordering::Acquire) {
            return;
        }
        let now = Instant::now();
        if now >= deadline {
            break;
        }
        std::thread::sleep(
            deadline
                .saturating_duration_since(now)
                .min(CANCEL_POLL_INTERVAL),
        );
    }
    state.fired.store(true, Ordering::Release);
    let waker = lock(&state.waker).take();
    if let Some(waker) = waker {
        waker.wake();
    }
}

#[cfg(target_os = "espidf")]
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
