//! Minimal executor for synchronous boundary code that must drive one future to
//! completion without depending on tokio or an embedded runtime.

use core::future::Future;
use core::task::{Context, Poll};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::task::{Wake, Waker};

/// Drive one future to completion, parking the current thread between wakeups.
pub fn block_on<F: Future>(future: F) -> F::Output {
    let parker = Arc::new(Parker {
        notified: Mutex::new(false),
        condvar: Condvar::new(),
    });
    let waker = Waker::from(Arc::clone(&parker));
    let mut context = Context::from_waker(&waker);
    let mut future = Box::pin(future);

    loop {
        *lock(&parker.notified) = false;
        if let Poll::Ready(output) = future.as_mut().poll(&mut context) {
            return output;
        }
        let mut notified = lock(&parker.notified);
        while !*notified {
            notified = wait(&parker.condvar, notified);
        }
    }
}

struct Parker {
    notified: Mutex<bool>,
    condvar: Condvar,
}

impl Wake for Parker {
    fn wake(self: Arc<Self>) {
        self.notify();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        self.notify();
    }
}

impl Parker {
    fn notify(&self) {
        *lock(&self.notified) = true;
        self.condvar.notify_one();
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
