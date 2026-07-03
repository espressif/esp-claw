//! Out-of-band control for an in-flight session drive.
//!
//! [`Orchestrator::deliver`](crate::orchestrator::Orchestrator::deliver) drives a
//! session to quiescence and, being `.await`ed to completion, cannot be signalled
//! from the outside once started. [`SessionControl`] is the escape hatch: the
//! driving layer creates one per drive, hands `&SessionControl` to
//! [`Orchestrator::deliver_interruptible`](crate::orchestrator::Orchestrator::deliver_interruptible),
//! and keeps a clone to signal from a concurrent task (e.g. the executor's
//! `select!` arm servicing new input while a drive is in flight).
//!
//! Everything is behind `Arc`/atomics so a shared `&SessionControl` can both be
//! observed by the drive loop and mutated by the control arm without a mutable
//! borrow.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use crate::agent::AgentAbortHandle;

/// Why a [`drive_interruptible`](crate::orchestrator::Orchestrator::deliver_interruptible)
/// loop stopped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DriveStop {
    /// No agent was ready: the drive ran to natural quiescence (the ordinary
    /// end of an `Append` delivery).
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
pub struct SessionControl {
    inner: Arc<ControlInner>,
}

#[derive(Default)]
struct ControlInner {
    interrupt: AtomicBool,
    cancel: AtomicBool,
    /// Abort handles for the session's currently-live agents, refreshed by the
    /// drive loop before each ready batch. A cancel request sets them so an
    /// in-flight LLM round is aborted cooperatively at its next checkpoint.
    abort_handles: Mutex<Vec<AgentAbortHandle>>,
}

impl SessionControl {
    /// A fresh control surface with no request pending and no live handles.
    pub fn new() -> Self {
        Self::default()
    }

    /// Request a graceful, whole-iteration interrupt: the in-flight iteration is
    /// left to finish and commit, then the drive loop stops with
    /// [`DriveStop::Interrupted`]. Does not touch the abort flag, so the current
    /// LLM/tool round is never cut short.
    pub fn request_interrupt(&self) {
        self.inner.interrupt.store(true, Ordering::Release);
    }

    /// Request a hard cancel: abort every currently-known in-flight round now
    /// (discarding its partial result), and stop the drive loop with
    /// [`DriveStop::Cancelled`] once the aborted round unwinds.
    pub fn request_cancel(&self) {
        self.inner.cancel.store(true, Ordering::Release);
        for handle in self
            .inner
            .abort_handles
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .iter()
        {
            handle.abort();
        }
    }

    /// Whether any interrupt or cancel has been requested. A cheap check for the
    /// driving layer to decide whether the completed drive needs special
    /// handling.
    pub fn is_requested(&self) -> bool {
        self.inner.interrupt.load(Ordering::Acquire) || self.inner.cancel.load(Ordering::Acquire)
    }

    /// Read-and-clear the interrupt request.
    pub(crate) fn take_interrupt(&self) -> bool {
        self.inner.interrupt.swap(false, Ordering::AcqRel)
    }

    /// Read-and-clear the cancel request.
    pub(crate) fn take_cancel(&self) -> bool {
        self.inner.cancel.swap(false, Ordering::AcqRel)
    }

    /// Replace the live abort handles a cancel request will fire. The drive loop
    /// refreshes these before each ready batch so a cancel aborts whatever agents
    /// are currently running (including subagents spawned mid-drive).
    pub(crate) fn refresh_abort_handles(&self, handles: Vec<AgentAbortHandle>) {
        *self
            .inner
            .abort_handles
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = handles;
    }
}
