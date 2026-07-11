use std::borrow::Cow;
use std::collections::{BTreeMap, HashMap};
use std::fmt;
use std::sync::{Arc, PoisonError, RwLock, RwLockReadGuard, RwLockWriteGuard};

use claw_checkpoint::{
    ChangePatternHint, CheckpointError, CheckpointStorageError, DurablePart, DurablePartError,
    DurableState, DurableStateCodec, PartGeneration, PartStateBlob, PartStateSlice, StorageHint,
    StorageSizeHint,
};
use serde::{Deserialize, Serialize};

use super::set::{ToolName, ToolSet};
use super::tool::Tool;

pub type ToolRegistryVersion = u64;
pub type ToolGroupId = String;
type CheckpointHook = Arc<
    dyn Fn(
            PartGeneration,
            PartStateBlob<'static>,
            StorageHint,
        ) -> Result<(), ToolRegistryCheckpointError>
        + Send
        + Sync,
>;
const TOOL_REGISTRY_STORAGE_HINT: StorageHint = StorageHint {
    size: StorageSizeHint::Small,
    change: ChangePatternHint::Arbitrary,
};

#[derive(Default)]
pub struct ToolRegistry {
    inner: RwLock<ToolRegistryInner>,
}

#[derive(Default)]
struct ToolRegistryInner {
    tools: HashMap<ToolName, Tool>,
    groups: HashMap<ToolGroupId, ToolGroupEntry>,
    state: DurableState<ToolRegistryState>,
    runtime_version: ToolRegistryVersion,
    checkpoint_hook: Option<CheckpointHook>,
}

#[derive(Clone)]
struct ToolGroupEntry {
    default_visibility: bool,
    tools: Vec<ToolName>,
}

#[derive(Clone, Debug, Default, Deserialize, PartialEq, Eq, Serialize)]
struct ToolRegistryState {
    started: bool,
    tools: BTreeMap<ToolName, bool>,
}

impl DurableStateCodec for ToolRegistryState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }
}

impl ToolRegistryInner {
    fn register_group(&mut self, group: ToolGroup) -> Result<(), ToolRegistryCheckpointError> {
        let previous_state = self.state.clone();
        let previous_runtime_version = self.runtime_version;
        let previous_tools = self.tools.clone();
        let previous_groups = self.groups.clone();
        let mut durable_changed = false;
        let default_visibility = group.default_visibility;
        let mut names = Vec::with_capacity(group.tools.len());

        for tool in group.tools {
            let name = tool.name().to_owned();
            if !self.state.get().tools.contains_key(&name) {
                self.state.get_mut().tools.insert(name.clone(), true);
                durable_changed = true;
            }
            self.tools.insert(name.clone(), tool);
            names.push(name);
        }
        self.groups.insert(
            group.id,
            ToolGroupEntry {
                default_visibility,
                tools: names,
            },
        );
        self.bump_runtime_version();
        if let Err(error) =
            self.checkpoint_or_restore(durable_changed, previous_state, previous_runtime_version)
        {
            self.tools = previous_tools;
            self.groups = previous_groups;
            return Err(error);
        }
        Ok(())
    }

    fn set_tool_enabled(
        &mut self,
        name: &str,
        enabled: bool,
    ) -> Result<(), ToolRegistryCheckpointError> {
        let previous_state = self.state.clone();
        let previous_runtime_version = self.runtime_version;
        if self.state.get().tools.get(name).copied() == Some(enabled) {
            return Ok(());
        }
        self.state.get_mut().tools.insert(name.to_owned(), enabled);
        self.bump_runtime_version();
        self.checkpoint_or_restore(true, previous_state, previous_runtime_version)
    }

    fn set_started(&mut self, started: bool) -> Result<(), ToolRegistryCheckpointError> {
        let previous_state = self.state.clone();
        let previous_runtime_version = self.runtime_version;
        if self.state.get().started == started {
            return Ok(());
        }
        self.state.get_mut().started = started;
        self.bump_runtime_version();
        self.checkpoint_or_restore(true, previous_state, previous_runtime_version)
    }

    fn checkpoint(&self, durable_changed: bool) -> Result<(), ToolRegistryCheckpointError> {
        if !durable_changed {
            return Ok(());
        }
        let Some(hook) = self.checkpoint_hook.as_ref() else {
            return Ok(());
        };
        let generation = self.state.generation();
        let state = self.state.export_state()?.into_owned();
        hook(generation, state, TOOL_REGISTRY_STORAGE_HINT)
    }

    fn checkpoint_or_restore(
        &mut self,
        durable_changed: bool,
        previous_state: DurableState<ToolRegistryState>,
        previous_runtime_version: ToolRegistryVersion,
    ) -> Result<(), ToolRegistryCheckpointError> {
        if let Err(error) = self.checkpoint(durable_changed) {
            self.state = previous_state;
            self.runtime_version = previous_runtime_version;
            return Err(error);
        }
        Ok(())
    }

    fn bump_runtime_version(&mut self) {
        self.runtime_version = self.runtime_version.saturating_add(1);
    }

    fn tool_projection(&self) -> ToolProjection {
        let state = self.state.get();
        if !state.started {
            return ToolProjection {
                registry_version: self.runtime_version,
                tools: Vec::new(),
            };
        }
        // Reverse index each tool to its owning group once, rather than
        // rescanning every group per tool.
        let mut group_of: HashMap<&ToolName, (&ToolGroupId, bool)> = HashMap::new();
        for (group_id, group) in &self.groups {
            for tool_name in &group.tools {
                group_of.insert(tool_name, (group_id, group.default_visibility));
            }
        }
        let mut tools = Vec::with_capacity(self.tools.len());
        for (name, tool) in &self.tools {
            if state.tools.get(name).copied() != Some(true) {
                continue;
            }
            let (group_id, default_visibility) = group_of
                .get(name)
                .map(|(group_id, visibility)| ((*group_id).clone(), *visibility))
                .unwrap_or_default();
            tools.push(ToolProjectionEntry {
                name: name.clone(),
                group_id,
                default_visibility,
                tool: tool.clone(),
            });
        }
        ToolProjection {
            registry_version: self.runtime_version,
            tools,
        }
    }
}

pub(super) struct ToolProjection {
    pub registry_version: ToolRegistryVersion,
    pub tools: Vec<ToolProjectionEntry>,
}

pub(super) struct ToolProjectionEntry {
    pub name: ToolName,
    pub group_id: ToolGroupId,
    pub default_visibility: bool,
    pub tool: Tool,
}

pub struct ToolGroup {
    pub(crate) id: ToolGroupId,
    pub(crate) default_visibility: bool,
    pub(crate) tools: Vec<Tool>,
}

impl ToolGroup {
    pub fn new(
        id: impl Into<ToolGroupId>,
        default_visibility: bool,
        tools: impl IntoIterator<Item = Tool>,
    ) -> Self {
        Self {
            id: id.into(),
            default_visibility,
            tools: tools.into_iter().collect(),
        }
    }

    pub fn id(&self) -> &str {
        &self.id
    }

    pub(crate) fn into_parts(self) -> (ToolGroupId, bool, Vec<Tool>) {
        (self.id, self.default_visibility, self.tools)
    }
}

#[derive(Debug, thiserror::Error)]
pub enum ToolRegistryError {
    #[error("tool already exists: {0}")]
    AlreadyExists(ToolName),
    #[error("tool group already exists: {0}")]
    GroupAlreadyExists(ToolGroupId),
    #[error("tool not found: {0}")]
    NotFound(ToolName),
    #[error("invalid tool: {0}")]
    InvalidTool(ToolName),
    #[error("invalid tool group: {0}")]
    InvalidGroup(ToolGroupId),
    #[error(transparent)]
    Checkpoint(#[from] ToolRegistryCheckpointError),
}

#[derive(Debug, thiserror::Error)]
pub enum ToolRegistryCheckpointError {
    #[error("checkpoint storage failed: coordinator failed: {0}")]
    Coordinator(#[from] CheckpointError),
    #[error("checkpoint storage failed: {0}")]
    Storage(#[from] CheckpointStorageError),
    #[error("checkpoint durable part failed: {0}")]
    Part(#[from] DurablePartError),
}

impl ToolRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register_group(&self, group: ToolGroup) -> Result<(), ToolRegistryError> {
        let mut inner = self.write_state();
        if group.id.is_empty() || group.tools.is_empty() {
            return Err(ToolRegistryError::InvalidGroup(group.id));
        }
        if inner.groups.contains_key(&group.id) {
            return Err(ToolRegistryError::GroupAlreadyExists(group.id));
        }
        for tool in &group.tools {
            let name = tool.name();
            if name.is_empty() {
                return Err(ToolRegistryError::InvalidTool(name.to_owned()));
            }
            if inner.tools.contains_key(name) {
                return Err(ToolRegistryError::AlreadyExists(name.to_owned()));
            }
        }

        if let Err(error) = inner.register_group(group) {
            return Err(error.into());
        }
        Ok(())
    }

    pub fn enable(&self, name: &str) -> Result<(), ToolRegistryError> {
        let mut inner = self.write_state();
        if !inner.tools.contains_key(name) {
            return Err(ToolRegistryError::NotFound(name.to_owned()));
        }

        if let Err(error) = inner.set_tool_enabled(name, true) {
            return Err(error.into());
        }
        Ok(())
    }

    pub fn disable(&self, name: &str) -> Result<(), ToolRegistryError> {
        let mut inner = self.write_state();
        if !inner.tools.contains_key(name) {
            return Err(ToolRegistryError::NotFound(name.to_owned()));
        }

        if let Err(error) = inner.set_tool_enabled(name, false) {
            return Err(error.into());
        }
        Ok(())
    }

    pub fn start_all(&self) -> Result<(), ToolRegistryError> {
        let mut inner = self.write_state();
        if let Err(error) = inner.set_started(true) {
            return Err(error.into());
        }
        Ok(())
    }

    pub fn stop_all(&self) -> Result<(), ToolRegistryError> {
        let mut inner = self.write_state();
        if let Err(error) = inner.set_started(false) {
            return Err(error.into());
        }
        Ok(())
    }

    pub fn tool_set(self: &Arc<Self>) -> ToolSet {
        ToolSet::new(self.clone())
    }

    pub fn tool_version(&self) -> ToolRegistryVersion {
        self.read_state().runtime_version
    }

    pub(super) fn contains_tool(&self, name: &str) -> bool {
        self.read_state().tools.contains_key(name)
    }

    pub(super) fn tool_projection(&self) -> ToolProjection {
        self.read_state().tool_projection()
    }

    fn read_state(&self) -> RwLockReadGuard<'_, ToolRegistryInner> {
        self.inner.read().unwrap_or_else(PoisonError::into_inner)
    }

    fn write_state(&self) -> RwLockWriteGuard<'_, ToolRegistryInner> {
        self.inner.write().unwrap_or_else(PoisonError::into_inner)
    }

    #[doc(hidden)]
    pub fn set_checkpoint_hook(
        &self,
        hook: impl Fn(
                PartGeneration,
                PartStateBlob<'static>,
                StorageHint,
            ) -> Result<(), ToolRegistryCheckpointError>
            + Send
            + Sync
            + 'static,
    ) {
        self.write_state().checkpoint_hook = Some(Arc::new(hook));
    }
}

impl DurablePart for ToolRegistry {
    fn name(&self) -> &'static str {
        "tool-registry"
    }

    fn generation(&self) -> PartGeneration {
        self.read_state().state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.read_state()
            .state
            .export_state()
            .map(PartStateBlob::into_owned)
    }

    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(Self {
            inner: RwLock::new(ToolRegistryInner {
                tools: HashMap::new(),
                groups: HashMap::new(),
                state: DurableState::restore_state(state)?,
                runtime_version: 0,
                checkpoint_hook: None,
            }),
        })
    }

    fn storage_hint(&self) -> StorageHint {
        TOOL_REGISTRY_STORAGE_HINT
    }
}

impl fmt::Debug for ToolRegistry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let inner = self.read_state();
        let state = inner.state.get();
        formatter
            .debug_struct("ToolRegistry")
            .field("tools", &inner.tools.len())
            .field("groups", &inner.groups.len())
            .field("started", &state.started)
            .field("runtime_version", &inner.runtime_version)
            .field("durable_generation", &inner.state.generation())
            .finish()
    }
}
