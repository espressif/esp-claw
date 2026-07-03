use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll, Waker};
use std::sync::{Mutex, MutexGuard};

use claw_api::ClawApiAsync;
use claw_interface::{ClawHttp, ClawTimer};

pub(super) struct SharedAsyncLlm<H: ClawHttp, Timer: ClawTimer> {
    state: Mutex<SharedAsyncLlmState<H, Timer>>,
}

struct SharedAsyncLlmState<H: ClawHttp, Timer: ClawTimer> {
    api: Option<ClawApiAsync<H, Timer>>,
    waker: Option<Waker>,
}

impl<H: ClawHttp, Timer: ClawTimer> SharedAsyncLlm<H, Timer> {
    pub(super) fn new(api: ClawApiAsync<H, Timer>) -> Self {
        Self {
            state: Mutex::new(SharedAsyncLlmState {
                api: Some(api),
                waker: None,
            }),
        }
    }

    pub(super) fn lease(&self) -> AsyncLlmLeaseFuture<'_, H, Timer> {
        AsyncLlmLeaseFuture { owner: self }
    }

    fn put(&self, api: ClawApiAsync<H, Timer>) {
        let waker = {
            let mut state = lock(&self.state);
            state.api = Some(api);
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

pub(super) struct AsyncLlmLeaseFuture<'owner, H: ClawHttp, Timer: ClawTimer> {
    owner: &'owner SharedAsyncLlm<H, Timer>,
}

impl<'owner, H: ClawHttp, Timer: ClawTimer> Future for AsyncLlmLeaseFuture<'owner, H, Timer> {
    type Output = AsyncLlmLease<'owner, H, Timer>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let mut state = lock(&self.owner.state);
        match state.api.take() {
            Some(api) => Poll::Ready(AsyncLlmLease {
                owner: self.owner,
                api: Some(api),
            }),
            None => {
                state.waker = Some(context.waker().clone());
                Poll::Pending
            }
        }
    }
}

pub(super) struct AsyncLlmLease<'owner, H: ClawHttp, Timer: ClawTimer> {
    owner: &'owner SharedAsyncLlm<H, Timer>,
    api: Option<ClawApiAsync<H, Timer>>,
}

impl<H: ClawHttp, Timer: ClawTimer> AsyncLlmLease<'_, H, Timer> {
    pub(super) fn api_mut(&mut self) -> &mut ClawApiAsync<H, Timer> {
        // Invariant: a live lease always owns the api until it is moved out in
        // `Drop`. `api` is only `None` after drop, when this method is
        // unreachable, so this is a bug marker, not an expected error path.
        self.api
            .as_mut()
            .expect("AsyncLlmLease holds its api until Drop")
    }
}

impl<H: ClawHttp, Timer: ClawTimer> Drop for AsyncLlmLease<'_, H, Timer> {
    fn drop(&mut self) {
        if let Some(api) = self.api.take() {
            self.owner.put(api);
        }
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
