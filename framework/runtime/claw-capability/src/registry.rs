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
            if let Err(error) = self.enable_group(&group_id) {
                return Err(self.rollback_failed_registration(&group_id, error));
            }
        }
        Ok(())
    }

    fn rollback_failed_registration(
        &self,
        group_id: &str,
        enable_error: CapabilityError,
    ) -> CapabilityError {
        match self.unregister_group(group_id) {
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
        let to_enable: Vec<String> = {
            let mut state = self.state();
            if state.started {
                return Ok(());
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
            if let Err(error) = self.enable_group(&group_id) {
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
            if let Err(error) = self.disable_group(&group_id) {
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
    /// On any lifecycle failure the group is left `Disabled` and the error is
    /// propagated.
    pub fn enable_group(&self, group_id: &str) -> Result<(), CapabilityError> {
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
    /// order), and mark it `Disabled`. The first stop error is returned after
    /// teardown completes.
    pub fn disable_group(&self, group_id: &str) -> Result<(), CapabilityError> {
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
    pub fn unregister_group(&self, group_id: &str) -> Result<(), CapabilityError> {
        if !self.state().groups.contains_key(group_id) {
            return Err(CapabilityError::NotFound);
        }
        let disable_result = self.disable_group(group_id);

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

    // --- Queries --------------------------------------------------------

    /// Whether a group is registered.
    pub fn group_exists(&self, group_id: &str) -> bool {
        self.state().groups.contains_key(group_id)
    }

    /// Whether a capability is registered.
    pub fn contains(&self, id: &str) -> bool {
        self.state().members.contains_key(id)
    }

    /// Current lifecycle state of a group.
    pub fn group_state(&self, group_id: &str) -> Result<CapabilityState, CapabilityError> {
        self.state()
            .groups
            .get(group_id)
            .map(|group| group.state)
            .ok_or(CapabilityError::NotFound)
    }

    /// Current lifecycle state of a capability (inherited from its group).
    pub fn state_of(&self, id: &str) -> Result<CapabilityState, CapabilityError> {
        let state = self.state();
        let member = state.members.get(id).ok_or(CapabilityError::NotFound)?;
        Ok(state.member_group_state(member))
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

    /// One [`Tool`] by capability id, if it is an available tool capability.
    pub fn tool(&self, id: &str) -> Option<Tool> {
        let state = self.state();
        let member = state.members.get(id)?;
        state
            .member_group_state(member)
            .is_available(state.started)
            .then(|| member.capability.as_tool().cloned())
            .flatten()
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

    /// One [`ChannelAdapter`] by capability id, if it is an available channel.
    pub fn channel(&self, id: &str) -> Option<Arc<dyn ChannelAdapter>> {
        let state = self.state();
        let member = state.members.get(id)?;
        state
            .member_group_state(member)
            .is_available(state.started)
            .then(|| member.capability.as_channel().cloned())
            .flatten()
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
        AsyncToolHandler, Tool, ToolFuture, ToolHandler, ToolInvocation, ToolInvokeError,
        ToolOutput,
    };

    use super::*;
    use crate::{Capability, CapabilityRole, OutboundMessage};

    /// A trivial tool whose id/name is `name`.
    struct DummyTool {
        name: String,
        schema: String,
    }

    impl DummyTool {
        fn tool(name: &str) -> Tool {
            Tool::new(Self {
                schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
                name: name.to_string(),
            })
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
        fn send(&self, message: &OutboundMessage) -> Result<(), CapabilityError> {
            self.sent.lock().unwrap().push(message.clone());
            Ok(())
        }
    }

    #[test]
    fn register_then_expose_tool() {
        let registry = Registry::new();
        registry
            .register(Capability::tool(DummyTool::tool("echo")))
            .unwrap();
        registry.start_all().unwrap();

        assert!(registry.contains("echo"));
        assert_eq!(registry.tools().len(), 1);
        let tool = registry.tool("echo").unwrap();
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
        let registry = Registry::new();
        registry
            .register(Capability::async_tool(AsyncDummyTool::new("async_echo")))
            .unwrap();
        registry.start_all().unwrap();

        assert_eq!(registry.tools().len(), 1);
        let tool = registry.tool("async_echo").unwrap();
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
            .register(Capability::tool(DummyTool::tool("dup")))
            .unwrap();
        assert_eq!(
            registry.register(Capability::tool(DummyTool::tool("dup"))),
            Err(CapabilityError::AlreadyExists)
        );
    }

    #[test]
    fn duplicate_member_within_group_conflicts() {
        let registry = Registry::new();
        let group = CapabilityGroup::new(
            "g",
            [
                Capability::tool(DummyTool::tool("a")),
                Capability::tool(DummyTool::tool("a")),
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
                [Capability::tool(DummyTool::tool("a"))]
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
            .register(
                Capability::new("svc", CapabilityRole::None).with_lifecycle(lifecycle.clone()),
            )
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
            .register(
                Capability::new("svc", CapabilityRole::None).with_lifecycle(lifecycle.clone()),
            )
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
                    [
                        Capability::tool(DummyTool::tool("run"))
                            .with_lifecycle(member_life.clone()),
                    ],
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
            .register(
                Capability::new("late", CapabilityRole::None).with_lifecycle(lifecycle.clone()),
            )
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

        assert!(!registry.contains("late"));
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

        assert!(!registry.contains("late"));
        assert!(!registry.group_exists("late"));
    }

    #[test]
    fn failing_start_leaves_group_disabled() {
        let registry = Registry::new();
        registry
            .register(
                Capability::new("svc", CapabilityRole::None).with_lifecycle(Arc::new(FailingStart)),
            )
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
                Capability::tool(DummyTool::tool("flaky")).with_lifecycle(Arc::new(FailingStart)),
            )
            .unwrap();

        assert_eq!(
            registry.start_all(),
            Err(CapabilityError::Failed("boom".into()))
        );

        assert!(registry.tools().is_empty());
        assert!(registry.tool("flaky").is_none());
    }

    #[test]
    fn registered_tools_are_visible_before_start_for_wiring() {
        let registry = Registry::new();
        registry
            .register(Capability::tool(DummyTool::tool("boot")))
            .unwrap();

        assert_eq!(registry.tools().len(), 1);
        assert!(registry.tool("boot").is_some());
    }

    #[test]
    fn disabled_group_hides_tools() {
        let registry = Registry::new();
        registry
            .register(Capability::tool(DummyTool::tool("t")))
            .unwrap();
        registry.start_all().unwrap();
        assert_eq!(registry.tools().len(), 1);

        registry.disable_group("t").unwrap();
        assert!(registry.tools().is_empty());
        assert!(registry.tool("t").is_none());
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
        let adapter = registry.channel("local").unwrap();
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
            .register(Capability::tool(DummyTool::tool("solo")))
            .unwrap();
        registry.unregister("solo").unwrap();
        assert!(!registry.contains("solo"));
        assert!(!registry.group_exists("solo"));
    }

    #[test]
    fn unregister_runs_deinit_after_init() {
        let lifecycle = Arc::new(CountingLifecycle::default());
        let registry = Registry::new();
        registry
            .register(
                Capability::new("svc", CapabilityRole::None).with_lifecycle(lifecycle.clone()),
            )
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
            .register(
                Capability::new("svc", CapabilityRole::None).with_lifecycle(lifecycle.clone()),
            )
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
                    Capability::tool(DummyTool::tool("x")),
                    Capability::tool(DummyTool::tool("y")),
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
}
