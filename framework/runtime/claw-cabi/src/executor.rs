use core::future::Future;
use core::task::{Context, Poll, Waker};
use std::sync::{Arc, Condvar, Mutex};
use std::task::Wake;

use edge_executor::LocalExecutor;

pub(crate) fn run<F>(future: F)
where
    F: Future<Output = ()>,
{
    let executor = LocalExecutor::<4>::new();
    block_on(executor.run(future));
}

fn block_on<F: Future>(future: F) -> F::Output {
    let parker = Arc::new(Parker::default());
    let waker = Waker::from(Arc::clone(&parker));
    let mut cx = Context::from_waker(&waker);
    let mut future = Box::pin(future);
    loop {
        match future.as_mut().poll(&mut cx) {
            Poll::Ready(output) => return output,
            Poll::Pending => parker.park(),
        }
    }
}

#[derive(Default)]
struct Parker {
    notified: Mutex<bool>,
    condvar: Condvar,
}

impl Parker {
    fn park(&self) {
        let mut notified = self
            .notified
            .lock()
            .unwrap_or_else(|p| p.into_inner());
        while !*notified {
            notified = self
                .condvar
                .wait(notified)
                .unwrap_or_else(|p| p.into_inner());
        }
        *notified = false;
    }
}

impl Wake for Parker {
    fn wake(self: Arc<Self>) {
        self.wake_by_ref();
    }

    fn wake_by_ref(self: &Arc<Self>) {
        let mut notified = self
            .notified
            .lock()
            .unwrap_or_else(|p| p.into_inner());
        *notified = true;
        self.condvar.notify_one();
    }
}
