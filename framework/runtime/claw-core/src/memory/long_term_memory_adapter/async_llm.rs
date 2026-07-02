use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll, Waker};
use std::sync::{Mutex, MutexGuard};

use claw_api::ClawApiAsync;
use claw_interface::{ClawHttpAsync, ClawTimer};

pub(super) struct SharedAsyncLlm<H: ClawHttpAsync, Timer: ClawTimer> {
    state: Mutex<SharedAsyncLlmState<H, Timer>>,
}

struct SharedAsyncLlmState<H: ClawHttpAsync, Timer: ClawTimer> {
    api: Option<ClawApiAsync<H, Timer>>,
    waker: Option<Waker>,
}

impl<H: ClawHttpAsync, Timer: ClawTimer> SharedAsyncLlm<H, Timer> {
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
        let mut state = lock(&self.state);
        state.api = Some(api);
        if let Some(waker) = state.waker.take() {
            waker.wake();
        }
    }
}

pub(super) struct AsyncLlmLeaseFuture<'owner, H: ClawHttpAsync, Timer: ClawTimer> {
    owner: &'owner SharedAsyncLlm<H, Timer>,
}

impl<'owner, H: ClawHttpAsync, Timer: ClawTimer> Future for AsyncLlmLeaseFuture<'owner, H, Timer> {
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

pub(super) struct AsyncLlmLease<'owner, H: ClawHttpAsync, Timer: ClawTimer> {
    owner: &'owner SharedAsyncLlm<H, Timer>,
    api: Option<ClawApiAsync<H, Timer>>,
}

impl<H: ClawHttpAsync, Timer: ClawTimer> AsyncLlmLease<'_, H, Timer> {
    pub(super) fn api_mut(&mut self) -> &mut ClawApiAsync<H, Timer> {
        match self.api.as_mut() {
            Some(api) => api,
            None => std::process::abort(),
        }
    }
}

impl<H: ClawHttpAsync, Timer: ClawTimer> Drop for AsyncLlmLease<'_, H, Timer> {
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
