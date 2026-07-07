//! Type-erased, poll-scoped invocation context for tool handlers.
//!
//! A tool handler runs deep inside an agent drive, far from whoever submitted the
//! request. Some handlers need per-submission data (a request id, an inbound
//! channel/chat id, …) that the drive itself is oblivious to. Rather than thread
//! that data through every agent/iteration layer, the driving layer wraps a
//! submission's drive future with [`with_context`]: on every `poll` the context
//! is installed into a thread-local, then restored on exit. A handler reads it
//! back with [`current_context`].
//!
//! The context is fully type-erased (`Arc<dyn Any + Send + Sync>`), so this crate
//! stays agnostic to what any particular caller stores. The setter (the drive
//! engine) and the reader (a concrete tool handler) agree on the concrete type;
//! [`current_context`] downcasts and yields `None` on a mismatch.
//!
//! Poll-scoped (not thread-scoped) so multiple submissions multiplexed on one
//! executor thread never clobber each other: each drive future carries its own
//! context and installs it only for the duration of its own `poll`.

use core::any::Any;
use core::cell::RefCell;
use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::sync::Arc;

/// A type-erased, shareable invocation context.
pub type SharedContext = Arc<dyn Any + Send + Sync>;

thread_local! {
    #[allow(clippy::missing_const_for_thread_local)]
    static CURRENT_CONTEXT: RefCell<Option<SharedContext>> = const { RefCell::new(None) };
}

/// Read the current invocation context and downcast it to `T`.
///
/// Returns `None` when no context is installed on this thread or the installed
/// context is not a `T`.
#[must_use]
pub fn current_context<T: Any + Send + Sync>() -> Option<Arc<T>> {
    CURRENT_CONTEXT
        .with(|slot| slot.borrow().clone())
        .and_then(|context| context.downcast::<T>().ok())
}

/// Wrap `future` so `context` is installed as the current invocation context for
/// the duration of each of its `poll`s.
///
/// A `None` context installs nothing (handlers see whatever, if anything, an
/// outer scope installed). The wrapper restores the previous context on every
/// poll exit, so nested and multiplexed scopes compose correctly.
pub fn with_context<F>(context: Option<SharedContext>, future: F) -> WithContext<F>
where
    F: Future,
{
    WithContext {
        context,
        future: Box::pin(future),
    }
}

/// The future returned by [`with_context`].
pub struct WithContext<F> {
    context: Option<SharedContext>,
    future: Pin<Box<F>>,
}

impl<F> Future for WithContext<F>
where
    F: Future,
{
    type Output = F::Output;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let _guard = ContextGuard::enter(self.context.clone());
        self.future.as_mut().poll(context)
    }
}

/// Installs a context on construction and restores the previous one on drop, so
/// an early return or panic inside the wrapped `poll` cannot leak a context.
struct ContextGuard {
    previous: Option<SharedContext>,
}

impl ContextGuard {
    fn enter(context: Option<SharedContext>) -> Self {
        let previous = CURRENT_CONTEXT.with(|slot| slot.replace(context));
        Self { previous }
    }
}

impl Drop for ContextGuard {
    fn drop(&mut self) {
        let previous = self.previous.take();
        CURRENT_CONTEXT.with(|slot| {
            let _ = slot.replace(previous);
        });
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use core::task::{Context, Poll};
    use std::sync::Arc;
    use std::task::Wake;

    #[derive(Debug, PartialEq, Eq)]
    struct Ctx {
        request_id: u32,
    }

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<T>(future: impl Future<Output = T>) -> T {
        let waker = std::task::Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        let mut future = std::pin::pin!(future);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    #[test]
    fn current_context_is_visible_inside_the_scope_and_gone_outside() {
        assert!(current_context::<Ctx>().is_none());

        let observed = block_on(with_context(Some(Arc::new(Ctx { request_id: 7 })), async {
            current_context::<Ctx>()
        }));
        assert_eq!(observed.as_deref(), Some(&Ctx { request_id: 7 }));

        // Restored to empty once the scope's future resolves.
        assert!(current_context::<Ctx>().is_none());
    }

    #[test]
    fn wrong_type_downcast_yields_none() {
        let observed = block_on(with_context(Some(Arc::new(9u64)), async {
            current_context::<Ctx>()
        }));
        assert!(observed.is_none());
    }
}
