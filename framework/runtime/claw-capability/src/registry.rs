//! The capability registry: registration, lifecycle, and role-based access.
//!
//! This is an *adapter*, not a second tool/channel runtime. It owns capability
//! identity and the lifecycle state machine, and it hands out the internal
//! representations — [`claw_tool::Tool`]s and [`ChannelAdapter`]s — that the
//! rest of the stack consumes. It deliberately holds **no** dispatch, schema, or
//! LLM-visibility logic: `claw-core` composes per-agent `ToolSet`s (with skills
//! / soft-hide) from the [`tools`](Registry::tools) it exposes.

use std::collections::{HashMap, HashSet};
use std::fmt;
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use claw_tool::Tool;

use crate::capability::{Capability, CapabilityGroup};
use crate::channel::ChannelAdapter;
use crate::error::CapabilityError;
use crate::lifecycle::{CapabilityState, Lifecycle};
use crate::observer::{CapabilityChange, CapabilityObserver};
use crate::state_store::CapabilityStateStore;

/// One registered capability plus its bookkeeping.
struct MemberEntry {
    capability: Capability,
    group_id: String,
    /// Whether this member's own lifecycle `init` has run (once, ever).
    init_called: bool,
}

/// One registered group plus its shared lifecycle and state.
struct GroupEntry {
    member_ids: Vec<String>,
    lifecycle: Option<Arc<dyn Lifecycle>>,
    /// Whether the group lifecycle `init` has run (once, ever).
    init_called: bool,
    state: CapabilityState,
}

#[derive(Default)]
struct RegistryState {
    groups: HashMap<String, GroupEntry>,
    members: HashMap<String, MemberEntry>,
    started: bool,
    /// Consumers notified after each mutation (empty = nobody listening).
    observers: Vec<Arc<dyn CapabilityObserver>>,
    /// Durable enable/disable deny-list backing, if injected.
    store: Option<Arc<dyn CapabilityStateStore>>,
}

impl RegistryState {
    /// State of the group a member belongs to (defaults to `Disabled` if the
    /// member or its group is somehow missing — treated as unavailable).
    fn member_group_state(&self, member: &MemberEntry) -> CapabilityState {
        self.groups
            .get(&member.group_id)
            .map(|group| group.state)
            .unwrap_or(CapabilityState::Disabled)
    }
}

/// Ordered lifecycle work gathered under the lock, run with the lock released.
struct StartPlan {
    group_lifecycle: Option<Arc<dyn Lifecycle>>,
    group_init_pending: bool,
    members: Vec<MemberPlan>,
}

struct MemberPlan {
    id: String,
    lifecycle: Option<Arc<dyn Lifecycle>>,
    init_pending: bool,
}

/// The capability registry. Construct with [`Registry::new`] / [`Default`],
/// register via [`register`](Registry::register) / [`register_group`](Registry::register_group),
/// drive lifecycle with [`start_all`](Registry::start_all) /
/// [`enable_group`](Registry::enable_group), and read out internal
/// representations via [`tools`](Registry::tools) / [`channels`](Registry::channels).
pub struct Registry {
    inner: Mutex<RegistryState>,
}

impl Default for Registry {
    fn default() -> Self {
        Self {
            inner: Mutex::new(RegistryState::default()),
        }
    }
}

impl fmt::Debug for Registry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let state = self.state();
        formatter
            .debug_struct("Registry")
            .field("group_count", &state.groups.len())
            .field("member_count", &state.members.len())
            .field("started", &state.started)
            .finish()
    }
}

impl Registry {
    /// An empty registry.
    pub fn new() -> Self {
        Self::default()
    }

    fn state(&self) -> MutexGuard<'_, RegistryState> {
        self.inner.lock().unwrap_or_else(PoisonError::into_inner)
    }

    // --- Wiring: observers & persistence (dependency-injected) ----------

    /// Attach a [`CapabilityObserver`] before sharing the registry (builder form).
    ///
    /// The observer is notified (lock released) after every real mutation. For an
    /// already-shared `Arc<Registry>`, use [`add_observer`](Self::add_observer).
    ///
    /// # Examples
    ///
    /// ```
    /// use std::sync::{Arc, Mutex};
    /// use claw_capability::{CapabilityChange, CapabilityObserver, Registry};
    ///
    /// #[derive(Default)]
    /// struct Log(Mutex<Vec<CapabilityChange>>);
    /// impl CapabilityObserver for Log {
    ///     fn on_change(&self, change: &CapabilityChange) {
    ///         self.0.lock().expect("lock").push(change.clone());
    ///     }
    /// }
    ///
    /// let registry = Registry::new().with_observer(Arc::new(Log::default()));
    /// let _ = registry; // register capabilities to see events.
    /// ```
    #[must_use]
    pub fn with_observer(self, observer: Arc<dyn CapabilityObserver>) -> Self {
        self.add_observer(observer);
        self
    }

    /// Attach a [`CapabilityObserver`] to an already-shared registry.
    pub fn add_observer(&self, observer: Arc<dyn CapabilityObserver>) {
        self.state().observers.push(observer);
    }

    /// Set the [`CapabilityStateStore`] the registry persists its enable/disable
    /// deny-list through (builder form).
    ///
    /// Set it before [`start_all`](Self::start_all) so the persisted overlay is
    /// applied at bring-up. See [`CapabilityStateStore`] for an example.
    #[must_use]
    pub fn with_state_store(self, store: Arc<dyn CapabilityStateStore>) -> Self {
        self.state().store = Some(store);
        self
    }

    /// Fire `change` on every observer with **no** registry lock held, so a
    /// callback may read back from the registry without deadlocking (the `Arc`
    /// clones are cheap).
    fn notify(&self, change: CapabilityChange) {
        let observers = self.state().observers.clone();
        for observer in &observers {
            observer.on_change(&change);
        }
    }

    fn store(&self) -> Option<Arc<dyn CapabilityStateStore>> {
        self.state().store.clone()
    }

    /// The persisted disabled deny-list (empty when no store is set).
    fn load_persisted_disabled(&self) -> Result<Vec<String>, CapabilityError> {
        match self.store() {
            Some(store) => store.load(),
            None => Ok(Vec::new()),
        }
    }

    /// Ensure `group_id` is absent from the persisted deny-list (enable intent);
    /// writes only when it was present.
    fn persist_enabled(&self, group_id: &str) -> Result<(), CapabilityError> {
        let Some(store) = self.store() else {
            return Ok(());
        };
        let mut disabled = store.load()?;
        if let Some(position) = disabled.iter().position(|id| id == group_id) {
            disabled.remove(position);
            store.save(&disabled)?;
        }
        Ok(())
    }

    /// Ensure `group_id` is present in the persisted deny-list (disable intent);
    /// writes only when it was absent.
    fn persist_disabled(&self, group_id: &str) -> Result<(), CapabilityError> {
        let Some(store) = self.store() else {
            return Ok(());
        };
        let mut disabled = store.load()?;
        if !disabled.iter().any(|id| id == group_id) {
            disabled.push(group_id.to_string());
            store.save(&disabled)?;
        }
        Ok(())
    }

    // --- Registration ---------------------------------------------------

    /// Register a single capability as a one-member group keyed by its id.
    pub fn register(&self, capability: Capability) -> Result<(), CapabilityError> {
        let id = capability.id().to_string();
        self.register_group(CapabilityGroup::new(id, [capability]))
    }

    /// Register a group of capabilities together.
    ///
    /// # Errors
    ///
    /// - [`CapabilityError::InvalidArg`] for an empty group id, empty members, or
    ///   an empty member id.
    /// - [`CapabilityError::AlreadyExists`] if the group id, or any member id,
    ///   already exists (including a duplicate within the group itself).
    ///
    /// If the registry is already started, the group is enabled immediately
    /// (its lifecycle runs), matching the historical auto-enable behavior. If
    /// that enable fails, registration is rolled back and the group is absent.
    pub fn register_group(&self, group: CapabilityGroup) -> Result<(), CapabilityError> {
        let (group_id, members, lifecycle) = group.into_parts();
        let was_started = {
            let mut state = self.state();
            Self::validate_group_parts(&state, &group_id, &members)?;

            let member_ids: Vec<String> = members
                .iter()
                .map(|member| member.id().to_string())
                .collect();
            for capability in members {
                let member_id = capability.id().to_string();
                state.members.insert(
                    member_id,
                    MemberEntry {
                        group_id: group_id.clone(),
                        init_called: false,
                        capability,
                    },
                );
            }
            state.groups.insert(
                group_id.clone(),
                GroupEntry {
                    member_ids,
                    lifecycle,
                    init_called: false,
                    state: CapabilityState::Registered,
                },
            );
            state.started
        };

        if was_started {
            if let Err(error) = self.enable_core(&group_id) {
                return Err(self.rollback_failed_registration(&group_id, error));
            }
        }
        self.notify(CapabilityChange::Registered(group_id));
        Ok(())
    }

    fn rollback_failed_registration(
        &self,
        group_id: &str,
        enable_error: CapabilityError,
    ) -> CapabilityError {
        match self.unregister_group_core(group_id) {
            Ok(()) => enable_error,
            Err(rollback_error) => CapabilityError::Failed(format!(
                "enable failed: {enable_error}; rollback failed: {rollback_error}"
            )),
        }
    }

    fn validate_group_parts(
        state: &RegistryState,
        group_id: &str,
        members: &[Capability],
    ) -> Result<(), CapabilityError> {
        if group_id.is_empty() || members.is_empty() {
            return Err(CapabilityError::InvalidArg);
        }
        if state.groups.contains_key(group_id) {
            return Err(CapabilityError::AlreadyExists);
        }
        let mut seen_ids: HashSet<&str> = HashSet::new();
        for member in members {
            if member.id().is_empty() {
                return Err(CapabilityError::InvalidArg);
            }
            if state.members.contains_key(member.id()) {
                return Err(CapabilityError::AlreadyExists);
            }
            if !seen_ids.insert(member.id()) {
                return Err(CapabilityError::AlreadyExists);
            }
        }
        Ok(())
    }

    // --- Lifecycle ------------------------------------------------------

    /// Start the registry and enable every non-disabled group.
    ///
    /// Returns the first enable error encountered, after attempting all groups.
    pub fn start_all(&self) -> Result<(), CapabilityError> {
        // Apply the persisted enable/disable overlay before deciding what to bring
        // up: any registered group the operator previously disabled is marked
        // `Disabled` so the enable loop skips it. Loaded with no lock held (it may
        // touch the filesystem).
        let persisted_disabled = self.load_persisted_disabled()?;
        let to_enable: Vec<String> = {
            let mut state = self.state();
            if state.started {
                return Ok(());
            }
            for group_id in &persisted_disabled {
                if state.groups.contains_key(group_id) {
                    Self::set_group_state(&mut state, group_id, CapabilityState::Disabled);
                }
            }
            state.started = true;
            state
                .groups
                .iter()
                .filter(|(_, group)| group.state != CapabilityState::Disabled)
                .map(|(id, _)| id.clone())
                .collect()
        };
        let mut first_error = None;
        for group_id in to_enable {
            if let Err(error) = self.enable_core(&group_id) {
                first_error.get_or_insert(error);
            }
        }
        first_error.map_or(Ok(()), Err)
    }

    /// Disable every started group and clear the started flag.
    ///
    /// Returns the first disable error encountered, after attempting all groups.
    pub fn stop_all(&self) -> Result<(), CapabilityError> {
        let to_disable: Vec<String> = {
            let state = self.state();
            state
                .groups
                .iter()
                .filter(|(_, group)| group.state == CapabilityState::Started)
                .map(|(id, _)| id.clone())
                .collect()
        };
        let mut first_error = None;
        for group_id in to_disable {
            if let Err(error) = self.disable_core(&group_id) {
                first_error.get_or_insert(error);
            }
        }
        self.state().started = false;
        first_error.map_or(Ok(()), Err)
    }

    /// Enable a group: run its lifecycle (group then members) and mark it
    /// `Started`. If the registry is not yet started, only marks it `Registered`
    /// (lifecycle runs later, at [`start_all`](Registry::start_all)).
    ///
    /// On success the group is removed from the persisted disabled deny-list (if a
    /// [`CapabilityStateStore`](crate::CapabilityStateStore) is set) and any
    /// [`CapabilityObserver`](crate::CapabilityObserver) is notified of the state
    /// change. On a lifecycle failure the group is left `Disabled` and the error
    /// is propagated (no persistence, no notification).
    ///
    /// # Errors
    ///
    /// - [`CapabilityError::NotFound`] if `group_id` is unknown.
    /// - A `Lifecycle` hook failure from `init`/`start`, propagated unchanged.
    /// - [`CapabilityError::Persistence`] if the deny-list write fails after the
    ///   group was already enabled in memory.
    pub fn enable_group(&self, group_id: &str) -> Result<(), CapabilityError> {
        let before = self.group_state(group_id).ok();
        self.enable_core(group_id)?;
        let after = self.group_state(group_id).ok();
        self.persist_enabled(group_id)?;
        if before != after {
            if let Some(state) = after {
                self.notify(CapabilityChange::StateChanged {
                    group_id: group_id.to_string(),
                    state,
                });
            }
        }
        Ok(())
    }

    /// The lifecycle work of [`enable_group`](Self::enable_group), without
    /// persistence or observer notification. Used by [`start_all`](Self::start_all)
    /// and [`register_group`](Self::register_group).
    fn enable_core(&self, group_id: &str) -> Result<(), CapabilityError> {
        let plan = {
            let mut state = self.state();
            let group = state
                .groups
                .get(group_id)
                .ok_or(CapabilityError::NotFound)?;
            if group.state == CapabilityState::Started {
                return Ok(());
            }
            if !state.started {
                Self::set_group_state(&mut state, group_id, CapabilityState::Registered);
                return Ok(());
            }
            Self::build_start_plan(&state, group_id)
        };

        match self.run_start_plan(group_id, plan) {
            Ok(()) => {
                Self::set_group_state(&mut self.state(), group_id, CapabilityState::Started);
                Ok(())
            }
            Err(error) => {
                Self::set_group_state(&mut self.state(), group_id, CapabilityState::Disabled);
                Err(error)
            }
        }
    }

    fn build_start_plan(state: &RegistryState, group_id: &str) -> StartPlan {
        let group = state.groups.get(group_id);
        let members = group
            .map(|group| {
                group
                    .member_ids
                    .iter()
                    .filter_map(|id| {
                        state.members.get(id).map(|member| MemberPlan {
                            id: id.clone(),
                            lifecycle: member.capability.lifecycle().cloned(),
                            init_pending: !member.init_called,
                        })
                    })
                    .collect()
            })
            .unwrap_or_default();
        StartPlan {
            group_lifecycle: group.and_then(|group| group.lifecycle.clone()),
            group_init_pending: group.is_some_and(|group| !group.init_called),
            members,
        }
    }

    /// Run a [`StartPlan`] with the registry lock released, marking each `init`
    /// as called only after it succeeds.
    fn run_start_plan(&self, group_id: &str, plan: StartPlan) -> Result<(), CapabilityError> {
        if let Some(lifecycle) = &plan.group_lifecycle {
            if plan.group_init_pending {
                lifecycle.init()?;
                self.mark_group_init_called(group_id);
            }
            lifecycle.start()?;
        }
        for member in &plan.members {
            let Some(lifecycle) = &member.lifecycle else {
                continue;
            };
            if member.init_pending {
                lifecycle.init()?;
                self.mark_member_init_called(&member.id);
            }
            lifecycle.start()?;
        }
        Ok(())
    }

    /// Disable a group: run member then group `stop` (best-effort, reverse
    /// order), and mark it `Disabled`.
    ///
    /// On success the group is added to the persisted disabled deny-list (if a
    /// [`CapabilityStateStore`](crate::CapabilityStateStore) is set) and any
    /// [`CapabilityObserver`](crate::CapabilityObserver) is notified. The first
    /// stop error is returned after teardown completes.
    ///
    /// # Errors
    ///
    /// - [`CapabilityError::NotFound`] if `group_id` is unknown.
    /// - The first `stop` hook failure, if any.
    /// - [`CapabilityError::Persistence`] if the deny-list write fails after the
    ///   group was already disabled in memory.
    pub fn disable_group(&self, group_id: &str) -> Result<(), CapabilityError> {
        let before = self.group_state(group_id).ok();
        self.disable_core(group_id)?;
        let after = self.group_state(group_id).ok();
        self.persist_disabled(group_id)?;
        if before != after {
            if let Some(state) = after {
                self.notify(CapabilityChange::StateChanged {
                    group_id: group_id.to_string(),
                    state,
                });
            }
        }
        Ok(())
    }

    /// The teardown work of [`disable_group`](Self::disable_group), without
    /// persistence or observer notification. Used by [`stop_all`](Self::stop_all)
    /// and [`unregister_group`](Self::unregister_group).
    fn disable_core(&self, group_id: &str) -> Result<(), CapabilityError> {
        let (group_lifecycle, member_lifecycles) = {
            let mut state = self.state();
            let group = state
                .groups
                .get(group_id)
                .ok_or(CapabilityError::NotFound)?;
            if group.state == CapabilityState::Disabled {
                return Ok(());
            }
            let group_lifecycle = group.lifecycle.clone();
            let member_lifecycles: Vec<Option<Arc<dyn Lifecycle>>> = group
                .member_ids
                .iter()
                .rev()
                .map(|id| {
                    state
                        .members
                        .get(id)
                        .and_then(|member| member.capability.lifecycle().cloned())
                })
                .collect();
            Self::set_group_state(&mut state, group_id, CapabilityState::Disabled);
            (group_lifecycle, member_lifecycles)
        };

        let mut first_error = None;
        for lifecycle in member_lifecycles.iter().flatten() {
            if let Err(error) = lifecycle.stop() {
                first_error.get_or_insert(error);
            }
        }
        if let Some(lifecycle) = &group_lifecycle {
            if let Err(error) = lifecycle.stop() {
                first_error.get_or_insert(error);
            }
        }
        first_error.map_or(Ok(()), Err)
    }

    /// Disable then remove a group and all its members, running the one-time
    /// `deinit` teardown (members then group, reverse order) for any whose
    /// `init` previously ran.
    ///
    /// On removal any [`CapabilityObserver`](crate::CapabilityObserver) is notified
    /// with [`CapabilityChange::Unregistered`](crate::CapabilityChange). Unregister
    /// does **not** touch the persisted disabled deny-list: a stale id left there
    /// is ignored at the next [`start_all`](Self::start_all) and pruned on the next
    /// enable/disable write.
    ///
    /// # Errors
    ///
    /// [`CapabilityError::NotFound`] if `group_id` is unknown; otherwise the first
    /// `stop`/`deinit` hook failure.
    pub fn unregister_group(&self, group_id: &str) -> Result<(), CapabilityError> {
        let existed = self.group_exists(group_id);
        let result = self.unregister_group_core(group_id);
        if existed && !self.group_exists(group_id) {
            self.notify(CapabilityChange::Unregistered(group_id.to_string()));
        }
        result
    }

    fn unregister_group_core(&self, group_id: &str) -> Result<(), CapabilityError> {
        if !self.state().groups.contains_key(group_id) {
            return Err(CapabilityError::NotFound);
        }
        let disable_result = self.disable_core(group_id);

        // Remove from the registry and collect the deinit hooks owed (init ran).
        let (group_deinit, member_deinits) = {
            let mut state = self.state();
            let Some(group) = state.groups.remove(group_id) else {
                return disable_result;
            };
            let member_deinits: Vec<Option<Arc<dyn Lifecycle>>> = group
                .member_ids
                .iter()
                .rev()
                .map(|id| {
                    state
                        .members
                        .remove(id)
                        .filter(|member| member.init_called)
                        .and_then(|member| member.capability.into_lifecycle())
                })
                .collect();
            let group_deinit = group.init_called.then_some(group.lifecycle).flatten();
            (group_deinit, member_deinits)
        };

        let mut first_error = disable_result.err();
        for lifecycle in member_deinits.iter().flatten() {
            if let Err(error) = lifecycle.deinit() {
                first_error.get_or_insert(error);
            }
        }
        if let Some(lifecycle) = &group_deinit {
            if let Err(error) = lifecycle.deinit() {
                first_error.get_or_insert(error);
            }
        }
        first_error.map_or(Ok(()), Err)
    }

    /// Unregister a single capability — only when it is the sole member of its
    /// group (multi-member groups must be removed as a whole).
    ///
    /// # Errors
    ///
    /// [`CapabilityError::NotFound`] if no capability has `id`;
    /// [`CapabilityError::InvalidState`] if it shares its group with others.
    pub fn unregister(&self, id: &str) -> Result<(), CapabilityError> {
        let group_id = {
            let state = self.state();
            let member = state.members.get(id).ok_or(CapabilityError::NotFound)?;
            let group_id = member.group_id.clone();
            let member_count = state
                .groups
                .get(&group_id)
                .map(|group| group.member_ids.len())
                .unwrap_or(0);
            if member_count != 1 {
                return Err(CapabilityError::InvalidState);
            }
            group_id
        };
        self.unregister_group(&group_id)
    }

    // --- State mutators (locked, tiny) ----------------------------------

    fn set_group_state(state: &mut RegistryState, group_id: &str, new_state: CapabilityState) {
        if let Some(group) = state.groups.get_mut(group_id) {
            group.state = new_state;
        }
    }

    fn mark_group_init_called(&self, group_id: &str) {
        if let Some(group) = self.state().groups.get_mut(group_id) {
            group.init_called = true;
        }
    }

    fn mark_member_init_called(&self, member_id: &str) {
        if let Some(member) = self.state().members.get_mut(member_id) {
            member.init_called = true;
        }
    }

    // --- Queries (crate-internal; lifecycle drivers read these) ---------

    /// Whether a group is registered.
    pub(crate) fn group_exists(&self, group_id: &str) -> bool {
        self.state().groups.contains_key(group_id)
    }

    /// Current lifecycle state of a group.
    pub(crate) fn group_state(&self, group_id: &str) -> Result<CapabilityState, CapabilityError> {
        self.state()
            .groups
            .get(group_id)
            .map(|group| group.state)
            .ok_or(CapabilityError::NotFound)
    }

    // --- Role-based access (the internal representations) ----------------

    /// Every available [`Tool`]-role capability, as cheap-to-clone
    /// [`claw_tool::Tool`]s, for `claw-core` to assemble into `ToolSet`s.
    /// Disabled groups are excluded.
    pub fn tools(&self) -> Vec<Tool> {
        let state = self.state();
        state
            .members
            .values()
            .filter(|member| state.member_group_state(member).is_available(state.started))
            .filter_map(|member| member.capability.as_tool().cloned())
            .collect()
    }

    /// One available tool-role [`Capability`] by id.
    ///
    /// Hands back the whole [`Capability`] (which `claw-core` decomposes into its
    /// internal `Tool` via [`CapabilityRole`](crate::CapabilityRole)) so the
    /// resolver seam never names the tool framework directly. Returns `None` for
    /// an unknown id, a disabled group, or a non-tool role.
    pub fn tool_capability(&self, id: &str) -> Option<Capability> {
        let state = self.state();
        let member = state.members.get(id)?;
        (state.member_group_state(member).is_available(state.started)
            && member.capability.as_tool().is_some())
        .then(|| member.capability.clone())
    }

    /// Every available [`Channel`](crate::CapabilityRole::Channel)-role adapter.
    pub fn channels(&self) -> Vec<Arc<dyn ChannelAdapter>> {
        let state = self.state();
        state
            .members
            .values()
            .filter(|member| state.member_group_state(member).is_available(state.started))
            .filter_map(|member| member.capability.as_channel().cloned())
            .collect()
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::task::{Wake, Waker};

    use claw_tool::{
        AsyncToolHandler, ToolFuture, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput,
    };

    use std::sync::Weak;

    use claw_interface::{ClawFs, MemFs};

    use super::*;
    use crate::{Capability, ChannelRuntime, FsCapabilityStateStore, OutboundMessage};

    /// A trivial tool whose id/name is `name`.
    struct DummyTool {
        name: String,
        schema: String,
    }

    impl DummyTool {
        fn named(name: &str) -> Self {
            Self {
                schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
                name: name.to_string(),
            }
        }
    }

    impl ToolHandler for DummyTool {
        fn name(&self) -> &str {
            &self.name
        }
        fn schema(&self) -> &str {
            &self.schema
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: format!("ran:{}", self.name),
                ok: true,
            })
        }
    }

    struct AsyncDummyTool {
        name: String,
        schema: String,
    }

    impl AsyncDummyTool {
        fn new(name: &str) -> Self {
            Self {
                schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
                name: name.to_string(),
            }
        }
    }

    impl AsyncToolHandler for AsyncDummyTool {
        fn name(&self) -> &str {
            &self.name
        }
        fn schema(&self) -> &str {
            &self.schema
        }
        fn invoke_async<'a>(&'a self, _call: &'a ToolInvocation<'_>) -> ToolFuture<'a> {
            Box::pin(async move {
                Ok(ToolOutput {
                    output: format!("async-ran:{}", self.name),
                    ok: true,
                })
            })
        }
    }

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    /// Counts lifecycle calls so tests can assert init-once / start / stop.
    #[derive(Default)]
    struct CountingLifecycle {
        init: AtomicUsize,
        start: AtomicUsize,
        stop: AtomicUsize,
        deinit: AtomicUsize,
    }

    impl Lifecycle for CountingLifecycle {
        fn init(&self) -> Result<(), CapabilityError> {
            self.init.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
        fn start(&self) -> Result<(), CapabilityError> {
            self.start.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
        fn stop(&self) -> Result<(), CapabilityError> {
            self.stop.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
        fn deinit(&self) -> Result<(), CapabilityError> {
            self.deinit.fetch_add(1, Ordering::SeqCst);
            Ok(())
        }
    }

    /// A lifecycle whose `start` always fails.
    struct FailingStart;
    impl Lifecycle for FailingStart {
        fn start(&self) -> Result<(), CapabilityError> {
            Err(CapabilityError::Failed("boom".into()))
        }
    }

    /// A lifecycle whose `start` and rollback `deinit` both fail.
    struct FailingStartAndDeinit;
    impl Lifecycle for FailingStartAndDeinit {
        fn init(&self) -> Result<(), CapabilityError> {
            Ok(())
        }
        fn start(&self) -> Result<(), CapabilityError> {
            Err(CapabilityError::Failed("start failed".into()))
        }
        fn deinit(&self) -> Result<(), CapabilityError> {
            Err(CapabilityError::Failed("deinit failed".into()))
        }
    }

    /// A minimal channel adapter that records sends.
    struct DummyChannel {
        id: String,
        sent: Mutex<Vec<OutboundMessage>>,
    }
    impl DummyChannel {
        fn new(id: &str) -> Arc<Self> {
            Arc::new(Self {
                id: id.to_string(),
                sent: Mutex::new(Vec::new()),
            })
        }
    }
    impl ChannelAdapter for DummyChannel {
        fn channel_id(&self) -> &str {
            &self.id
        }

        fn open(&self, _runtime: Arc<dyn ChannelRuntime>) -> Result<(), CapabilityError> {
            Ok(())
        }

        fn close(&self) -> Result<(), CapabilityError> {
            Ok(())
        }

        fn send(&self, message: &OutboundMessage) -> Result<(), CapabilityError> {
            self.sent.lock().unwrap().push(message.clone());
            Ok(())
        }
    }

    /// Records every change the registry pushes to it.
    #[derive(Default)]
    struct RecordingObserver {
        changes: Mutex<Vec<CapabilityChange>>,
    }
    impl CapabilityObserver for RecordingObserver {
        fn on_change(&self, change: &CapabilityChange) {
            self.changes.lock().unwrap().push(change.clone());
        }
    }

    /// Reads back from the registry inside the callback — hangs if `notify` still
    /// holds the registry lock when it fires.
    #[derive(Default)]
    struct ReentrantObserver {
        registry: Mutex<Weak<Registry>>,
        tool_counts: Mutex<Vec<usize>>,
    }
    impl CapabilityObserver for ReentrantObserver {
        fn on_change(&self, _change: &CapabilityChange) {
            if let Some(registry) = self.registry.lock().unwrap().upgrade() {
                let count = registry.tools().len();
                self.tool_counts.lock().unwrap().push(count);
            }
        }
    }

    /// A store whose `save` always fails, to prove persistence errors propagate.
    struct FailingStore;
    impl CapabilityStateStore for FailingStore {
        fn load(&self) -> Result<Vec<String>, CapabilityError> {
            Ok(Vec::new())
        }
        fn save(&self, _disabled_groups: &[String]) -> Result<(), CapabilityError> {
            Err(CapabilityError::Persistence("disk full".into()))
        }
    }

    fn fs_store(fs: MemFs, path: &str) -> Arc<dyn CapabilityStateStore> {
        Arc::new(FsCapabilityStateStore::new(fs, path.to_string()))
    }

    #[test]
    fn register_then_expose_tool() {
        let registry = Registry::new();
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("echo"))))
            .unwrap();
        registry.start_all().unwrap();

        assert!(registry.tool_capability("echo").is_some());
        assert_eq!(registry.tools().len(), 1);
        let tool = registry
            .tools()
            .into_iter()
            .find(|tool| tool.name() == "echo")
            .unwrap();
        let output = tool
            .invoke(&ToolInvocation {
                id: None,
                name: "echo",
                arguments_json: "{}",
            })
            .unwrap();
        assert_eq!(output.output, "ran:echo");
    }

    #[test]
    fn register_then_expose_async_tool() {
        claw_tool::init_tool_executor(claw_interface::StdThread).unwrap();

        let registry = Registry::new();
        registry
            .register(Capability::from_tool(Tool::new_async(AsyncDummyTool::new(
                "async_echo",
            ))))
            .unwrap();
        registry.start_all().unwrap();

        assert_eq!(registry.tools().len(), 1);
        let tool = registry
            .tools()
            .into_iter()
            .find(|tool| tool.name() == "async_echo")
            .unwrap();
        let output = block_on(tool.invoke_async(&ToolInvocation {
            id: None,
            name: "async_echo",
            arguments_json: "{}",
        }))
        .unwrap();
        assert_eq!(output.output, "async-ran:async_echo");
    }

    #[test]
    fn duplicate_id_conflicts() {
        let registry = Registry::new();
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("dup"))))
            .unwrap();
        assert_eq!(
            registry.register(Capability::from_tool(Tool::new(DummyTool::named("dup")))),
            Err(CapabilityError::AlreadyExists)
        );
    }

    #[test]
    fn duplicate_member_within_group_conflicts() {
        let registry = Registry::new();
        let group = CapabilityGroup::new(
            "g",
            [
                Capability::from_tool(Tool::new(DummyTool::named("a"))),
                Capability::from_tool(Tool::new(DummyTool::named("a"))),
            ],
        );
        assert_eq!(
            registry.register_group(group),
            Err(CapabilityError::AlreadyExists)
        );
    }

    #[test]
    fn empty_id_or_members_rejected() {
        let registry = Registry::new();
        assert_eq!(
            registry.register_group(CapabilityGroup::new(
                "",
                [Capability::from_tool(Tool::new(DummyTool::named("a")))]
            )),
            Err(CapabilityError::InvalidArg)
        );
        assert_eq!(
            registry.register_group(CapabilityGroup::new("g", [])),
            Err(CapabilityError::InvalidArg)
        );
    }

    #[test]
    fn lifecycle_runs_on_start_and_stop() {
        let lifecycle = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        registry
            .register(Capability::none("svc").with_lifecycle(lifecycle.clone()))
            .unwrap();

        // Not started yet: nothing runs.
        assert_eq!(lifecycle.start.load(Ordering::SeqCst), 0);

        registry.start_all().unwrap();
        assert_eq!(lifecycle.init.load(Ordering::SeqCst), 1);
        assert_eq!(lifecycle.start.load(Ordering::SeqCst), 1);

        registry.disable_group("svc").unwrap();
        assert_eq!(lifecycle.stop.load(Ordering::SeqCst), 1);
        assert_eq!(
            registry.group_state("svc").unwrap(),
            CapabilityState::Disabled
        );
    }

    #[test]
    fn reenable_starts_again_without_reinitializing() {
        let lifecycle = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        registry
            .register(Capability::none("svc").with_lifecycle(lifecycle.clone()))
            .unwrap();
        registry.start_all().unwrap();
        registry.disable_group("svc").unwrap();
        registry.enable_group("svc").unwrap();

        // init once across enable/disable/enable; start each enable.
        assert_eq!(lifecycle.init.load(Ordering::SeqCst), 1);
        assert_eq!(lifecycle.start.load(Ordering::SeqCst), 2);
        assert_eq!(lifecycle.stop.load(Ordering::SeqCst), 1);
        assert_eq!(
            registry.group_state("svc").unwrap(),
            CapabilityState::Started
        );
    }

    #[test]
    fn group_lifecycle_runs_before_members() {
        // Shared group runtime backing a tool (the cap_lua shape).
        let group_life = Arc::new(CountingLifecycle::default());
        let member_life = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        registry
            .register_group(
                CapabilityGroup::new(
                    "lua",
                    [Capability::from_tool(Tool::new(DummyTool::named("run")))
                        .with_lifecycle(member_life.clone())],
                )
                .with_lifecycle(group_life.clone()),
            )
            .unwrap();
        registry.start_all().unwrap();

        assert_eq!(group_life.start.load(Ordering::SeqCst), 1);
        assert_eq!(member_life.start.load(Ordering::SeqCst), 1);
        assert_eq!(registry.tools().len(), 1);
    }

    #[test]
    fn register_while_started_auto_enables() {
        let lifecycle = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        registry.start_all().unwrap();
        registry
            .register(Capability::none("late").with_lifecycle(lifecycle.clone()))
            .unwrap();

        assert_eq!(lifecycle.start.load(Ordering::SeqCst), 1);
        assert_eq!(
            registry.group_state("late").unwrap(),
            CapabilityState::Started
        );
    }

    #[test]
    fn register_while_started_rolls_back_on_start_failure() {
        let registry = Registry::new();
        registry.start_all().unwrap();

        assert_eq!(
            registry.register(Capability::none("late").with_lifecycle(Arc::new(FailingStart)),),
            Err(CapabilityError::Failed("boom".into()))
        );

        assert!(!registry.group_exists("late"));
    }

    #[test]
    fn register_while_started_reports_rollback_failure() {
        let registry = Registry::new();
        registry.start_all().unwrap();

        assert_eq!(
            registry.register(
                Capability::none("late")
                    .with_lifecycle(Arc::new(FailingStartAndDeinit)),
            ),
            Err(CapabilityError::Failed(
                "enable failed: capability operation failed: start failed; \
                 rollback failed: capability operation failed: deinit failed"
                    .into()
            ))
        );

        assert!(!registry.group_exists("late"));
    }

    #[test]
    fn failing_start_leaves_group_disabled() {
        let registry = Registry::new();
        registry
            .register(Capability::none("svc").with_lifecycle(Arc::new(FailingStart)))
            .unwrap();
        assert_eq!(
            registry.start_all(),
            Err(CapabilityError::Failed("boom".into()))
        );
        assert_eq!(
            registry.group_state("svc").unwrap(),
            CapabilityState::Disabled
        );
    }

    #[test]
    fn failed_started_group_hides_tool_role() {
        let registry = Registry::new();
        registry
            .register(
                Capability::from_tool(Tool::new(DummyTool::named("flaky")))
                    .with_lifecycle(Arc::new(FailingStart)),
            )
            .unwrap();

        assert_eq!(
            registry.start_all(),
            Err(CapabilityError::Failed("boom".into()))
        );

        assert!(registry.tools().is_empty());
        assert!(registry.tool_capability("flaky").is_none());
    }

    #[test]
    fn registered_tools_are_visible_before_start_for_wiring() {
        let registry = Registry::new();
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("boot"))))
            .unwrap();

        assert_eq!(registry.tools().len(), 1);
        assert!(registry.tool_capability("boot").is_some());
    }

    #[test]
    fn disabled_group_hides_tools() {
        let registry = Registry::new();
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("t"))))
            .unwrap();
        registry.start_all().unwrap();
        assert_eq!(registry.tools().len(), 1);

        registry.disable_group("t").unwrap();
        assert!(registry.tools().is_empty());
        assert!(registry.tool_capability("t").is_none());
    }

    #[test]
    fn channel_role_is_exposed_and_sends() {
        let channel = DummyChannel::new("local");
        let registry = Registry::new();
        registry
            .register(
                Capability::channel(channel.clone())
                    .with_lifecycle(Arc::new(CountingLifecycle::default())),
            )
            .unwrap();
        registry.start_all().unwrap();

        assert!(registry.tools().is_empty());
        assert_eq!(registry.channels().len(), 1);
        let adapter = registry
            .channels()
            .into_iter()
            .find(|channel| channel.channel_id() == "local")
            .unwrap();
        adapter
            .send(&OutboundMessage {
                channel: "local".into(),
                chat_id: "c1".into(),
                text: "hi".into(),
                reply_to_message_id: None,
            })
            .unwrap();
        assert_eq!(channel.sent.lock().unwrap().len(), 1);
    }

    #[test]
    fn unregister_single_member_then_gone() {
        let registry = Registry::new();
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("solo"))))
            .unwrap();
        registry.unregister("solo").unwrap();
        assert!(!registry.group_exists("solo"));
    }

    #[test]
    fn unregister_runs_deinit_after_init() {
        let lifecycle = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        registry
            .register(Capability::none("svc").with_lifecycle(lifecycle.clone()))
            .unwrap();
        registry.start_all().unwrap();
        registry.unregister("svc").unwrap();

        // Full symmetric cycle: init -> start -> stop -> deinit, each once.
        assert_eq!(lifecycle.init.load(Ordering::SeqCst), 1);
        assert_eq!(lifecycle.start.load(Ordering::SeqCst), 1);
        assert_eq!(lifecycle.stop.load(Ordering::SeqCst), 1);
        assert_eq!(lifecycle.deinit.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn unregister_without_init_skips_deinit() {
        let lifecycle = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        // Registered but never started, so init never ran.
        registry
            .register(Capability::none("svc").with_lifecycle(lifecycle.clone()))
            .unwrap();
        registry.unregister("svc").unwrap();

        assert_eq!(lifecycle.init.load(Ordering::SeqCst), 0);
        assert_eq!(lifecycle.deinit.load(Ordering::SeqCst), 0);
    }

    #[test]
    fn unregister_member_of_multi_group_is_rejected() {
        let registry = Registry::new();
        registry
            .register_group(CapabilityGroup::new(
                "pair",
                [
                    Capability::from_tool(Tool::new(DummyTool::named("x"))),
                    Capability::from_tool(Tool::new(DummyTool::named("y"))),
                ],
            ))
            .unwrap();
        assert_eq!(registry.unregister("x"), Err(CapabilityError::InvalidState));
    }

    #[test]
    fn enable_disable_unknown_group_errors() {
        let registry = Registry::new();
        assert_eq!(
            registry.enable_group("nope"),
            Err(CapabilityError::NotFound)
        );
        assert_eq!(
            registry.disable_group("nope"),
            Err(CapabilityError::NotFound)
        );
        assert_eq!(registry.group_state("nope"), Err(CapabilityError::NotFound));
    }

    // --- Observer -------------------------------------------------------

    #[test]
    fn observer_sees_one_change_per_real_mutation() {
        let observer = Arc::new(RecordingObserver::default());
        let registry = Registry::new().with_observer(observer.clone());

        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("t"))))
            .unwrap();
        registry.start_all().unwrap(); // start_all does not notify per group
        registry.disable_group("t").unwrap();
        registry.disable_group("t").unwrap(); // no-op: already Disabled
        registry.enable_group("t").unwrap();
        registry.enable_group("t").unwrap(); // no-op: already Started
        registry.unregister("t").unwrap();

        let changes = observer.changes.lock().unwrap().clone();
        assert_eq!(
            changes,
            vec![
                CapabilityChange::Registered("t".into()),
                CapabilityChange::StateChanged {
                    group_id: "t".into(),
                    state: CapabilityState::Disabled,
                },
                CapabilityChange::StateChanged {
                    group_id: "t".into(),
                    state: CapabilityState::Started,
                },
                CapabilityChange::Unregistered("t".into()),
            ]
        );
    }

    #[test]
    fn observer_callback_runs_without_registry_lock() {
        let observer = Arc::new(ReentrantObserver::default());
        let registry = Arc::new(Registry::new());
        *observer.registry.lock().unwrap() = Arc::downgrade(&registry);
        registry.add_observer(observer.clone());

        // Each of these fires the callback, which locks the registry again. If
        // `notify` held the lock, this test would deadlock.
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("a"))))
            .unwrap();
        registry.start_all().unwrap();
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("b"))))
            .unwrap(); // started -> auto-enable, tools() now sees both

        let counts = observer.tool_counts.lock().unwrap().clone();
        // Registered("a") saw 1 tool; Registered("b") saw 2.
        assert_eq!(counts, vec![1, 2]);
    }

    // --- Persistence ----------------------------------------------------

    #[test]
    fn disabled_state_survives_restart() {
        let fs = MemFs::new();

        let registry = Registry::new().with_state_store(fs_store(fs.clone(), "state.json"));
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("t"))))
            .unwrap();
        registry.start_all().unwrap();
        registry.disable_group("t").unwrap();

        // Fresh registry, same backing store: the group replays as Disabled.
        let restarted = Registry::new().with_state_store(fs_store(fs.clone(), "state.json"));
        restarted
            .register(Capability::from_tool(Tool::new(DummyTool::named("t"))))
            .unwrap();
        restarted.start_all().unwrap();
        assert_eq!(
            restarted.group_state("t").unwrap(),
            CapabilityState::Disabled
        );
        assert!(restarted.tools().is_empty());

        // Re-enabling clears the deny-list, so the next restart is Started.
        restarted.enable_group("t").unwrap();
        let after_enable = Registry::new().with_state_store(fs_store(fs.clone(), "state.json"));
        after_enable
            .register(Capability::from_tool(Tool::new(DummyTool::named("t"))))
            .unwrap();
        after_enable.start_all().unwrap();
        assert_eq!(
            after_enable.group_state("t").unwrap(),
            CapabilityState::Started
        );
    }

    #[test]
    fn persisted_unknown_group_is_ignored_at_start() {
        let fs = MemFs::new();
        fs.write_atomic("state.json", br#"["ghost"]"#).unwrap();

        let registry = Registry::new().with_state_store(fs_store(fs.clone(), "state.json"));
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("real"))))
            .unwrap();
        registry.start_all().unwrap();

        assert_eq!(
            registry.group_state("real").unwrap(),
            CapabilityState::Started
        );
    }

    #[test]
    fn save_failure_propagates_from_disable() {
        let registry = Registry::new().with_state_store(Arc::new(FailingStore));
        registry
            .register(Capability::from_tool(Tool::new(DummyTool::named("t"))))
            .unwrap();
        registry.start_all().unwrap();

        assert!(matches!(
            registry.disable_group("t"),
            Err(CapabilityError::Persistence(_))
        ));
    }
}
