//! Change notification for the capability [`Registry`](crate::Registry).
//!
//! The registry is the source of truth for which capabilities exist and whether
//! they are available. A long-lived consumer (e.g. `claw-core`, which snapshots
//! the registry's tools into a per-agent `ToolSet`) needs to know when that set
//! changes so it can rebuild. A [`CapabilityObserver`] is the dependency-injected
//! seam for that signal: the registry calls [`on_change`](CapabilityObserver::on_change)
//! **after** every real mutation, with the registry lock released, so a callback
//! may freely read back from the registry without deadlocking.
//!
//! The observer only reports *that* the set changed. It carries no expectation of
//! an incremental patch: the consumer is expected to rebuild wholesale (a fresh
//! `ToolSet` / a re-read of [`channels`](crate::Registry::channels)) at whatever
//! boundary is safe for it (for the agent, the next turn).

use crate::lifecycle::CapabilityState;

/// A change to the registry's set of capabilities or their availability.
///
/// Emitted by the [`Registry`](crate::Registry) after a successful mutation.
/// A consumer that only rebuilds wholesale can treat every variant identically
/// ("something changed, rebuild"); the payload is provided for logging and for
/// consumers that want to react selectively.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum CapabilityChange {
    /// A group (by id) was registered.
    Registered(String),
    /// A group (by id) was unregistered.
    Unregistered(String),
    /// A registered group (by id) transitioned to a new lifecycle state
    /// (e.g. enabled to `Started` or disabled to `Disabled`).
    StateChanged {
        /// The group whose state changed.
        group_id: String,
        /// The group's new state.
        state: CapabilityState,
    },
}

/// Observer notified after the registry's available capability set changes.
///
/// Register an implementation with
/// [`Registry::with_observer`](crate::Registry::with_observer) /
/// [`Registry::add_observer`](crate::Registry::add_observer). Callbacks run with
/// no registry lock held, so `on_change` may read back from the registry.
///
/// # Examples
///
/// ```
/// use std::sync::{Arc, Mutex};
/// use claw_capability::{CapabilityChange, CapabilityObserver, Registry};
///
/// #[derive(Default)]
/// struct ChangeLog(Mutex<Vec<CapabilityChange>>);
/// impl CapabilityObserver for ChangeLog {
///     fn on_change(&self, change: &CapabilityChange) {
///         self.0.lock().expect("lock").push(change.clone());
///     }
/// }
///
/// let log = Arc::new(ChangeLog::default());
/// let _registry = Registry::new().with_observer(log.clone());
/// // Registering/enabling/disabling/unregistering now appends to `log`.
/// ```
pub trait CapabilityObserver: Send + Sync {
    /// Called after a registry mutation, with the registry lock released.
    fn on_change(&self, change: &CapabilityChange);
}
