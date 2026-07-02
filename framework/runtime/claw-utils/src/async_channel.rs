//! Small single-consumer async channel used where pulling in a runtime-specific
//! channel would couple the core crates to a particular executor.

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll, Waker};
use std::collections::VecDeque;
use std::sync::{Arc, Mutex, MutexGuard};

/// Create an unbounded multi-producer, single-consumer async channel.
pub fn async_channel<T>() -> (AsyncSender<T>, AsyncReceiver<T>) {
    let shared = Arc::new(Mutex::new(ChannelState {
        queue: VecDeque::new(),
        closed: false,
        sender_count: 1,
        receiver_waker: None,
    }));
    (
        AsyncSender {
            shared: Arc::clone(&shared),
        },
        AsyncReceiver { shared },
    )
}

/// Error returned when sending into a closed async channel.
#[derive(Debug, PartialEq, Eq)]
pub struct AsyncSendError<T>(pub T);

/// Sending side of [`async_channel`].
pub struct AsyncSender<T> {
    shared: Arc<Mutex<ChannelState<T>>>,
}

/// Receiving side of [`async_channel`].
pub struct AsyncReceiver<T> {
    shared: Arc<Mutex<ChannelState<T>>>,
}

struct ChannelState<T> {
    queue: VecDeque<T>,
    closed: bool,
    sender_count: usize,
    receiver_waker: Option<Waker>,
}

impl<T> AsyncSender<T> {
    /// Queue one value and wake the receiver if it is waiting.
    pub fn send(&self, value: T) -> Result<(), AsyncSendError<T>> {
        let waker = {
            let mut state = lock(&self.shared);
            if state.closed {
                return Err(AsyncSendError(value));
            }
            state.queue.push_back(value);
            state.receiver_waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
        Ok(())
    }
}

impl<T> Clone for AsyncSender<T> {
    fn clone(&self) -> Self {
        let mut state = lock(&self.shared);
        state.sender_count = state.sender_count.saturating_add(1);
        drop(state);
        Self {
            shared: Arc::clone(&self.shared),
        }
    }
}

impl<T> Drop for AsyncSender<T> {
    fn drop(&mut self) {
        let waker = {
            let mut state = lock(&self.shared);
            state.sender_count = state.sender_count.saturating_sub(1);
            if state.sender_count == 0 {
                state.closed = true;
                state.receiver_waker.take()
            } else {
                None
            }
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

impl<T> AsyncReceiver<T> {
    /// Receive the next queued value, or `None` after the channel closes and the
    /// queue drains.
    pub fn recv(&self) -> AsyncRecv<'_, T> {
        AsyncRecv { receiver: self }
    }

    /// Close the channel. Already queued values can still be drained.
    pub fn close(&self) {
        let waker = {
            let mut state = lock(&self.shared);
            state.closed = true;
            state.receiver_waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

impl<T> Drop for AsyncReceiver<T> {
    fn drop(&mut self) {
        self.close();
    }
}

/// Future returned by [`AsyncReceiver::recv`].
pub struct AsyncRecv<'receiver, T> {
    receiver: &'receiver AsyncReceiver<T>,
}

impl<T> Future for AsyncRecv<'_, T> {
    type Output = Option<T>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let mut state = lock(&self.receiver.shared);
        if let Some(value) = state.queue.pop_front() {
            return Poll::Ready(Some(value));
        }
        if state.closed || state.sender_count == 0 {
            return Poll::Ready(None);
        }
        state.receiver_waker = Some(context.waker().clone());
        Poll::Pending
    }
}

/// Create a one-shot async reply channel.
pub fn async_oneshot<T>() -> (AsyncOneshotSender<T>, AsyncOneshotReceiver<T>) {
    let shared = Arc::new(Mutex::new(OneshotState {
        value: None,
        closed: false,
        waker: None,
    }));
    (
        AsyncOneshotSender {
            shared: Some(Arc::clone(&shared)),
        },
        AsyncOneshotReceiver { shared },
    )
}

/// Sending side of [`async_oneshot`].
pub struct AsyncOneshotSender<T> {
    shared: Option<Arc<Mutex<OneshotState<T>>>>,
}

/// Receiving side of [`async_oneshot`].
pub struct AsyncOneshotReceiver<T> {
    shared: Arc<Mutex<OneshotState<T>>>,
}

struct OneshotState<T> {
    value: Option<T>,
    closed: bool,
    waker: Option<Waker>,
}

impl<T> AsyncOneshotSender<T> {
    /// Complete the one-shot with a value.
    pub fn send(mut self, value: T) -> Result<(), AsyncSendError<T>> {
        let Some(shared) = self.shared.take() else {
            return Err(AsyncSendError(value));
        };
        let waker = {
            let mut state = lock(&shared);
            if state.closed || state.value.is_some() {
                return Err(AsyncSendError(value));
            }
            state.value = Some(value);
            state.closed = true;
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
        Ok(())
    }
}

impl<T> Drop for AsyncOneshotSender<T> {
    fn drop(&mut self) {
        let Some(shared) = self.shared.take() else {
            return;
        };
        let waker = {
            let mut state = lock(&shared);
            state.closed = true;
            state.waker.take()
        };
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

impl<T> Future for AsyncOneshotReceiver<T> {
    type Output = Option<T>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let mut state = lock(&self.shared);
        if let Some(value) = state.value.take() {
            return Poll::Ready(Some(value));
        }
        if state.closed {
            return Poll::Ready(None);
        }
        state.waker = Some(context.waker().clone());
        Poll::Pending
    }
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[cfg(test)]
mod tests {
    use super::{async_channel, async_oneshot};
    use crate::block_on;
    use std::thread;
    use std::time::Duration;

    #[test]
    fn channel_drains_queued_values_then_closes() {
        let (sender, receiver) = async_channel();
        sender.send(1).expect("send first value");
        sender.send(2).expect("send second value");
        drop(sender);

        block_on(async {
            assert_eq!(receiver.recv().await, Some(1));
            assert_eq!(receiver.recv().await, Some(2));
            assert_eq!(receiver.recv().await, None);
        });
    }

    #[test]
    fn channel_wakes_waiting_receiver() {
        let (sender, receiver) = async_channel();
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(10));
            sender.send(7).expect("send delayed value");
        });

        assert_eq!(block_on(receiver.recv()), Some(7));
    }

    #[test]
    fn oneshot_wakes_waiting_receiver() {
        let (sender, receiver) = async_oneshot();
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(10));
            sender.send("done").expect("send one-shot value");
        });

        assert_eq!(block_on(receiver), Some("done"));
    }
}
