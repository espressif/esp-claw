use async_channel::{Receiver, Sender};

use claw_api::ClawApiAsync;
use claw_interface::{ClawHttp, ClawTimer};

/// Private shared lease helper used by the concrete memory-side LLM adapters.
pub(super) struct SharedAsyncLlm<H: ClawHttp, Timer: ClawTimer> {
    api_tx: Sender<ClawApiAsync<H, Timer>>,
    api_rx: Receiver<ClawApiAsync<H, Timer>>,
}

impl<H: ClawHttp, Timer: ClawTimer> SharedAsyncLlm<H, Timer> {
    pub(super) fn new(api: ClawApiAsync<H, Timer>) -> Self {
        let (api_tx, api_rx) = async_channel::bounded(1);
        api_tx
            .try_send(api)
            .expect("a new single-item LLM channel has capacity");
        Self { api_tx, api_rx }
    }

    pub(super) async fn lease(&self) -> AsyncLlmLease<'_, H, Timer> {
        let api = self
            .api_rx
            .recv()
            .await
            .expect("SharedAsyncLlm owns the channel sender");
        AsyncLlmLease {
            owner: self,
            api: Some(api),
        }
    }
}

pub(super) struct AsyncLlmLease<'owner, H: ClawHttp, Timer: ClawTimer> {
    owner: &'owner SharedAsyncLlm<H, Timer>,
    api: Option<ClawApiAsync<H, Timer>>,
}

impl<H: ClawHttp, Timer: ClawTimer> AsyncLlmLease<'_, H, Timer> {
    pub(super) fn api_mut(&mut self) -> &mut ClawApiAsync<H, Timer> {
        self.api
            .as_mut()
            .expect("AsyncLlmLease holds its api until Drop")
    }
}

impl<H: ClawHttp, Timer: ClawTimer> Drop for AsyncLlmLease<'_, H, Timer> {
    fn drop(&mut self) {
        if let Some(api) = self.api.take() {
            self.owner
                .api_tx
                .try_send(api)
                .expect("only one AsyncLlmLease can exist at a time");
        }
    }
}

#[cfg(test)]
mod tests {
    use core::future::Future;
    use core::pin::Pin;
    use core::task::{Context, Poll, Waker};
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;
    use std::task::Wake;

    use claw_api::ClawApiAsync;
    use claw_interface::{BlockingHttpAdapter, ImmediateTimer, NoopHttp};

    use super::SharedAsyncLlm;

    #[derive(Default)]
    struct ReadyFlag(AtomicBool);

    impl ReadyFlag {
        fn take(&self) -> bool {
            self.0.swap(false, Ordering::AcqRel)
        }
    }

    impl Wake for ReadyFlag {
        fn wake(self: Arc<Self>) {
            self.0.store(true, Ordering::Release);
        }

        fn wake_by_ref(self: &Arc<Self>) {
            self.0.store(true, Ordering::Release);
        }
    }

    fn poll_with<F: Future>(future: Pin<&mut F>, ready: &Arc<ReadyFlag>) -> Poll<F::Output> {
        let waker = Waker::from(Arc::clone(ready));
        future.poll(&mut Context::from_waker(&waker))
    }

    #[test]
    fn every_independent_lease_waiter_makes_progress() {
        let api = ClawApiAsync::new(BlockingHttpAdapter::new(NoopHttp), ImmediateTimer);
        let shared = SharedAsyncLlm::new(api);
        let holder = futures_lite::future::block_on(shared.lease());

        let ready = [
            Arc::new(ReadyFlag::default()),
            Arc::new(ReadyFlag::default()),
        ];
        let mut waiters = [Box::pin(shared.lease()), Box::pin(shared.lease())];
        for (waiter, ready) in waiters.iter_mut().zip(&ready) {
            assert!(poll_with(waiter.as_mut(), ready).is_pending());
        }

        drop(holder);

        let mut completed = [false; 2];
        for _ in 0..waiters.len() {
            let mut polled = false;
            for ((waiter, ready), completed) in waiters.iter_mut().zip(&ready).zip(&mut completed) {
                if !*completed && ready.take() {
                    polled = true;
                    if let Poll::Ready(lease) = poll_with(waiter.as_mut(), ready) {
                        *completed = true;
                        drop(lease);
                    }
                }
            }
            if completed.iter().all(|completed| *completed) {
                break;
            }
            if !polled {
                break;
            }
        }

        assert!(
            completed.into_iter().all(|completed| completed),
            "returning each lease must eventually wake every independent waiter"
        );
    }
}
