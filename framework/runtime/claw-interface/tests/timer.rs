#![cfg(feature = "timermock")]

use core::future::Future;
use core::sync::atomic::{AtomicBool, Ordering};
use core::task::{Context, Poll};
use std::task::Waker;
use std::time::Duration;

use claw_interface::{Cancel, ClawTimer, ImmediateTimer, SleepOutcome, YieldingTimer};

#[test]
fn immediate_timer_completes_without_waiting() {
    let mut timer = ImmediateTimer;
    let abort = AtomicBool::new(false);
    let outcome = block_on_counting(timer.sleep(Duration::from_millis(500), Cancel::new(&abort))).0;

    assert_eq!(outcome, SleepOutcome::Completed);
    assert!(outcome.is_completed());
}

#[test]
fn timer_reports_pre_cancelled_token() {
    let mut timer = ImmediateTimer;
    let abort = AtomicBool::new(true);
    let outcome = block_on_counting(timer.sleep(Duration::from_millis(500), Cancel::new(&abort))).0;

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

fn block_on_counting<F: Future>(future: F) -> (F::Output, u32) {
    let mut future = Box::pin(future);
    let mut context = Context::from_waker(Waker::noop());
    let mut polls = 0;
    loop {
        polls += 1;
        if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
            return (output, polls);
        }
    }
}
