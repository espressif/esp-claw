use core::future::Future;
use core::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};

use embedded_executor::{AllocExecutor, Sleep, Wake};
use lock_api::{GuardSend, RawMutex};

type CabiExecutor<'a> = AllocExecutor<'a, RawSpinlock, ParkingSleep>;

pub(crate) fn run<F>(future: F)
where
    F: Future<Output = ()>,
{
    let mut executor: CabiExecutor<'_> = AllocExecutor::new();
    executor.spawn(future);
    executor.run();
}

struct RawSpinlock(AtomicBool);

// SAFETY: the flag gives one exclusive critical section with acquire/release.
unsafe impl RawMutex for RawSpinlock {
    #[allow(clippy::declare_interior_mutable_const)]
    const INIT: Self = Self(AtomicBool::new(false));
    type GuardMarker = GuardSend;

    fn lock(&self) {
        while !self.try_lock() {
            core::hint::spin_loop();
        }
    }

    fn try_lock(&self) -> bool {
        self.0
            .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
    }

    unsafe fn unlock(&self) {
        self.0.store(false, Ordering::Release);
    }
}

#[derive(Clone, Default)]
struct ParkingSleep {
    inner: Arc<ParkingState>,
}

#[derive(Default)]
struct ParkingState {
    notified: Mutex<bool>,
    condvar: Condvar,
}

impl Wake for ParkingSleep {
    fn wake(&self) {
        let mut notified = lock(&self.inner.notified);
        *notified = true;
        self.inner.condvar.notify_one();
    }
}

impl ParkingSleep {
    fn park(&self) {
        let mut notified = lock(&self.inner.notified);
        while !*notified {
            notified = wait(&self.inner.condvar, notified);
        }
        *notified = false;
    }
}

impl Sleep for ParkingSleep {
    fn sleep(&self) {
        self.park();
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn wait<'a, T>(condvar: &Condvar, guard: MutexGuard<'a, T>) -> MutexGuard<'a, T> {
    condvar
        .wait(guard)
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
