//! ESP-IDF timer implementation for async retry backoff.
//!
//! Backed by the shared `esp_timer` one-shot software-timer service: a single
//! system timer task dispatches every callback, so a `sleep` costs one timer
//! object — never a spawned thread. This matters because `sleep` is on the retry
//! backoff and (via `ClawTimer`) the FFI receive-timeout paths, both of which can
//! fire often; spawning a thread per sleep would churn tasks/stacks.

#[cfg(target_os = "espidf")]
use claw_interface::{Cancel, ClawTimer, SleepOutcome, TimerFuture};
#[cfg(target_os = "espidf")]
use core::ffi::{c_char, c_int, c_void};
#[cfg(target_os = "espidf")]
use core::{future::Future, pin::Pin, task::Context, task::Poll, time::Duration};
#[cfg(target_os = "espidf")]
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex, MutexGuard,
};
#[cfg(target_os = "espidf")]
use std::task::Waker;

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

// --- esp_timer FFI ----------------------------------------------------------

#[cfg(target_os = "espidf")]
const ESP_OK: c_int = 0;

/// `esp_timer_dispatch_t::ESP_TIMER_TASK`: dispatch the callback from the shared
/// `esp_timer` service task (not an ISR).
#[cfg(target_os = "espidf")]
const ESP_TIMER_TASK: c_int = 0;

#[cfg(target_os = "espidf")]
type EspTimerHandle = *mut c_void;
#[cfg(target_os = "espidf")]
type EspTimerCb = unsafe extern "C" fn(*mut c_void);

// Field order mirrors `esp_timer_create_args_t` in esp_timer.h.
#[cfg(target_os = "espidf")]
#[repr(C)]
struct EspTimerCreateArgs {
    callback: Option<EspTimerCb>,
    arg: *mut c_void,
    dispatch_method: c_int,
    name: *const c_char,
    skip_unhandled_events: bool,
}

#[cfg(target_os = "espidf")]
extern "C" {
    fn esp_timer_create(
        create_args: *const EspTimerCreateArgs,
        out_handle: *mut EspTimerHandle,
    ) -> c_int;
    fn esp_timer_start_once(timer: EspTimerHandle, timeout_us: u64) -> c_int;
    fn esp_timer_stop(timer: EspTimerHandle) -> c_int;
    fn esp_timer_delete(timer: EspTimerHandle) -> c_int;
}

/// Shared between the future and the `esp_timer` callback. An extra `Arc` strong
/// count is handed to the timer as its `arg` so the callback's pointer stays
/// valid across the FFI boundary; it is reclaimed in `Drop` once the timer has
/// been stopped and deleted.
#[cfg(target_os = "espidf")]
struct SleepState {
    fired: AtomicBool,
    waker: Mutex<Option<Waker>>,
}

#[cfg(target_os = "espidf")]
struct EspIdfSleep<'cancel> {
    cancel: Cancel<'cancel>,
    duration: Duration,
    state: Arc<SleepState>,
    /// `Some` once the one-shot timer has been created and armed. Its presence
    /// also tracks the extra `Arc` strong count handed to the timer.
    handle: Option<EspTimerHandle>,
}

#[cfg(target_os = "espidf")]
impl<'cancel> EspIdfSleep<'cancel> {
    fn new(duration: Duration, cancel: Cancel<'cancel>) -> Self {
        Self {
            cancel,
            duration,
            state: Arc::new(SleepState {
                fired: AtomicBool::new(duration == Duration::ZERO),
                waker: Mutex::new(None),
            }),
            handle: None,
        }
    }

    /// Create and arm the one-shot timer. Returns `false` if the `esp_timer`
    /// service rejects the request, so the caller can resolve the sleep as
    /// `Completed` (skip the backoff) instead of hanging — a create/arm failure
    /// is surfaced, never fatal.
    fn start(&mut self) -> bool {
        if self.handle.is_some() {
            return true;
        }
        // Hand an extra strong count to the callback via `arg`; reclaimed in
        // `Drop` once the timer is stopped and deleted.
        let arg = Arc::into_raw(Arc::clone(&self.state)) as *mut c_void;
        let create_args = EspTimerCreateArgs {
            callback: Some(timer_callback),
            arg,
            dispatch_method: ESP_TIMER_TASK,
            name: b"claw_timer\0".as_ptr() as *const c_char,
            skip_unhandled_events: false,
        };
        let mut handle: EspTimerHandle = core::ptr::null_mut();
        if unsafe { esp_timer_create(&create_args, &mut handle) } != ESP_OK {
            // No timer owns the strong count we just leaked; reclaim it.
            drop(unsafe { Arc::from_raw(arg as *const SleepState) });
            return false;
        }
        let timeout_us = self.duration.as_micros().min(u128::from(u64::MAX)) as u64;
        if unsafe { esp_timer_start_once(handle, timeout_us) } != ESP_OK {
            unsafe {
                esp_timer_delete(handle);
                drop(Arc::from_raw(arg as *const SleepState));
            }
            return false;
        }
        self.handle = Some(handle);
        true
    }
}

#[cfg(target_os = "espidf")]
impl Future for EspIdfSleep<'_> {
    type Output = SleepOutcome;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        if self.cancel.is_cancelled() {
            return Poll::Ready(SleepOutcome::Cancelled);
        }
        if self.state.fired.load(Ordering::Acquire) {
            return Poll::Ready(SleepOutcome::Completed);
        }
        // Record the latest waker before arming so a fire that races the arm is
        // never lost.
        *lock(&self.state.waker) = Some(context.waker().clone());
        if !self.start() {
            // Could not arm the timer; resolve as completed so the caller
            // proceeds without waiting rather than stalling forever.
            return Poll::Ready(SleepOutcome::Completed);
        }
        // The timer may have fired between storing the waker and arming; re-check.
        if self.state.fired.load(Ordering::Acquire) {
            return Poll::Ready(SleepOutcome::Completed);
        }
        Poll::Pending
    }
}

#[cfg(target_os = "espidf")]
impl Drop for EspIdfSleep<'_> {
    fn drop(&mut self) {
        if let Some(handle) = self.handle.take() {
            // Stop + delete first: for `ESP_TIMER_TASK` dispatch `esp_timer_delete`
            // waits for any in-flight callback to finish, so no callback can run
            // (or touch `state`) after this returns. Only then reclaim the strong
            // count handed to the timer.
            unsafe {
                esp_timer_stop(handle);
                esp_timer_delete(handle);
                drop(Arc::from_raw(Arc::as_ptr(&self.state)));
            }
        }
    }
}

/// One-shot `esp_timer` callback (runs on the shared timer task): mark the sleep
/// fired and wake its task. The strong count backing `arg` is owned by the timer
/// until `Drop` reclaims it, so the pointer is valid here.
#[cfg(target_os = "espidf")]
extern "C" fn timer_callback(arg: *mut c_void) {
    if arg.is_null() {
        return;
    }
    let state = unsafe { &*(arg as *const SleepState) };
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
