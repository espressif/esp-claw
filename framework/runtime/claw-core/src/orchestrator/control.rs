//! Out-of-band control for an in-flight session drive.
//!
//! [`Orchestrator::submit`](crate::orchestrator::Orchestrator::submit) owns
//! per-session delivery. While a session drive is awaiting LLM/tool work, the
//! orchestrator stores a [`SessionControl`] for that drive so the active
//! [`SubmitStream`](crate::orchestrator::SubmitStream) can request an interrupt
//! or cancel without exposing the drive protocol outside this module.
//!
//! Everything is behind `Arc`/atomics so a shared `&SessionControl` can both be
//! observed by the drive loop and mutated by the control arm without a mutable
//! borrow.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

type CancelHook = Arc<dyn Fn() + Send + Sync + 'static>;

/// Why an interruptible instance drive stopped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum DriveStop {
    /// No agent was ready: the drive ran to natural quiescence.
    Quiescent,
    /// A graceful interrupt was honoured: the in-flight iteration finished and
    /// committed, then the loop stopped before starting the next one.
    Interrupted,
    /// A hard cancel was honoured: the in-flight LLM round was aborted (its
    /// partial result discarded) and the loop stopped.
    Cancelled,
}

/// A cloneable, out-of-band control surface for a session's in-flight drive.
///
/// See the [module docs](self). Cloning shares one underlying state, so the
/// drive loop and a concurrent control arm coordinate through the same flags.
#[derive(Clone, Default)]
pub(crate) struct SessionControl {
    inner: Arc<ControlInner>,
}

#[derive(Default)]
struct ControlInner {
    interrupt: AtomicBool,
    cancel: AtomicBool,
    /// Action for aborting the currently in-flight work. The drive loop replaces
    /// this before each ready batch; `request_cancel` only calls it and stays
    /// decoupled from the concrete agent implementation.
    cancel_hook: Mutex<Option<CancelHook>>,
}

impl SessionControl {
    /// A fresh control surface with no request pending and no cancel hook.
    pub(crate) fn new() -> Self {
        Self::default()
    }

    /// Request a graceful, whole-iteration interrupt: the in-flight iteration is
    /// left to finish and commit, then the drive loop stops with
    /// [`DriveStop::Interrupted`]. Does not touch the abort flag, so the current
    /// LLM/tool round is never cut short.
    pub(crate) fn request_interrupt(&self) {
        self.inner.interrupt.store(true, Ordering::Release);
    }

    /// Request a hard cancel: fire the currently registered in-flight cancel
    /// hook, then stop the drive loop with [`DriveStop::Cancelled`] once the
    /// aborted round unwinds.
    pub(crate) fn request_cancel(&self) {
        self.inner.cancel.store(true, Ordering::Release);
        let hook = self
            .inner
            .cancel_hook
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .clone();
        if let Some(hook) = hook {
            hook();
        }
    }

    /// Read-and-clear the interrupt request.
    pub(crate) fn take_interrupt(&self) -> bool {
        self.inner.interrupt.swap(false, Ordering::AcqRel)
    }

    /// Read-and-clear the cancel request.
    pub(crate) fn take_cancel(&self) -> bool {
        self.inner.cancel.swap(false, Ordering::AcqRel)
    }

    /// Replace the action a cancel request will fire. The drive loop refreshes
    /// this before each ready batch so a cancel aborts whatever work is currently
    /// running (including subagents spawned mid-drive).
    pub(crate) fn set_cancel_hook<F>(&self, hook: F)
    where
        F: Fn() + Send + Sync + 'static,
    {
        let hook: CancelHook = Arc::new(hook);
        let should_fire = self.inner.cancel.load(Ordering::Acquire);
        *self
            .inner
            .cancel_hook
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = Some(Arc::clone(&hook));
        if should_fire {
            hook();
        }
    }

    /// Clear any batch-local cancel action once the drive has no in-flight batch.
    pub(crate) fn clear_cancel_hook(&self) {
        *self
            .inner
            .cancel_hook
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = None;
    }
}
