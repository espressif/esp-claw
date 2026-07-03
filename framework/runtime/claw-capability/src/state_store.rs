//! Persistence of the capability enable/disable overlay.
//!
//! Firmware registers a *static* set of capability groups at boot; that set is
//! not persisted (it comes from code). What an operator can change at runtime is
//! whether a group is administratively disabled. [`CapabilityStateStore`] is the
//! dependency-injected seam that survives a reboot: it stores the **deny-list**
//! of disabled group ids. The [`Registry`](crate::Registry) applies it once at
//! [`start_all`](crate::Registry::start_all) (matching registered groups are left
//! `Disabled`) and rewrites it on every
//! [`enable_group`](crate::Registry::enable_group) /
//! [`disable_group`](crate::Registry::disable_group).
//!
//! A deny-list (rather than a full state map) is deliberately minimal: only the
//! non-default choices ("the operator turned these off") are recorded. Ids left
//! in the file for groups the current firmware no longer registers are ignored at
//! load and pruned on the next write.

use claw_interface::ClawFs;

use crate::error::CapabilityError;

/// Persists the set of administratively-disabled capability group ids.
///
/// Implement this to back the registry's enable/disable overlay with durable
/// storage, and inject it via
/// [`Registry::with_state_store`](crate::Registry::with_state_store). The default
/// (no store) keeps the registry purely in-memory. [`FsCapabilityStateStore`] is
/// the [`ClawFs`]-backed production implementation.
///
/// # Examples
///
/// An in-memory store (as a test double would define):
///
/// ```
/// use std::sync::Mutex;
/// use claw_capability::{CapabilityStateStore, Registry};
///
/// #[derive(Default)]
/// struct MemStore(Mutex<Vec<String>>);
/// impl CapabilityStateStore for MemStore {
///     fn load(&self) -> Result<Vec<String>, claw_capability::CapabilityError> {
///         Ok(self.0.lock().expect("lock").clone())
///     }
///     fn save(&self, disabled_groups: &[String]) -> Result<(), claw_capability::CapabilityError> {
///         *self.0.lock().expect("lock") = disabled_groups.to_vec();
///         Ok(())
///     }
/// }
///
/// let _registry = Registry::new().with_state_store(std::sync::Arc::new(MemStore::default()));
/// ```
pub trait CapabilityStateStore: Send + Sync {
    /// Return the persisted deny-list (the group ids marked disabled). A missing
    /// backing store (e.g. first boot, no file yet) is an empty list, not an
    /// error.
    ///
    /// # Errors
    ///
    /// [`CapabilityError::Persistence`] if the backing store cannot be read or
    /// parsed.
    fn load(&self) -> Result<Vec<String>, CapabilityError>;

    /// Durably replace the persisted deny-list with `disabled_groups`.
    ///
    /// # Errors
    ///
    /// [`CapabilityError::Persistence`] if the write fails.
    fn save(&self, disabled_groups: &[String]) -> Result<(), CapabilityError>;
}

/// A [`CapabilityStateStore`] backed by a [`ClawFs`], storing the deny-list as a
/// JSON array of group ids at a fixed path, written atomically.
///
/// The path is [DATA-rooted](crate) by the caller (this type does not join
/// roots). A missing file loads as an empty deny-list.
///
/// ```ignore
/// // Device/host wiring (needs a concrete `ClawFs`, e.g. `DiskFs`):
/// let store = FsCapabilityStateStore::new(fs, "capabilities/state.json");
/// let registry = Registry::new().with_state_store(std::sync::Arc::new(store));
/// ```
pub struct FsCapabilityStateStore<F: ClawFs> {
    fs: F,
    path: String,
}

impl<F: ClawFs> FsCapabilityStateStore<F> {
    /// Persist the deny-list at `path` on `fs`.
    pub fn new(fs: F, path: impl Into<String>) -> Self {
        Self {
            fs,
            path: path.into(),
        }
    }
}

impl<F: ClawFs> CapabilityStateStore for FsCapabilityStateStore<F> {
    fn load(&self) -> Result<Vec<String>, CapabilityError> {
        if !self.fs.exists(&self.path) {
            return Ok(Vec::new());
        }
        let bytes = self
            .fs
            .read(&self.path)
            .map_err(|error| CapabilityError::Persistence(error.to_string()))?;
        serde_json::from_slice::<Vec<String>>(&bytes)
            .map_err(|error| CapabilityError::Persistence(error.to_string()))
    }

    fn save(&self, disabled_groups: &[String]) -> Result<(), CapabilityError> {
        let bytes = serde_json::to_vec(disabled_groups)
            .map_err(|error| CapabilityError::Persistence(error.to_string()))?;
        self.fs
            .write_atomic(&self.path, &bytes)
            .map_err(|error| CapabilityError::Persistence(error.to_string()))
    }
}
