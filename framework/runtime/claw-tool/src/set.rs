use std::borrow::Cow;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::sync::Arc;

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use claw_permission::Action;
use serde::{Deserialize, Serialize};

use super::registry::{ToolRegistry, ToolRegistryVersion};
use super::tool::{Tool, ToolError, ToolInvocation, ToolOutput, ToolResult};

pub type ToolName = String;

const NO_SCHEMAS: &str = "no schemas";
const NO_TOOL_CONTEXT: &str = "no tool context";
const NO_EXTRA_TOOL_CONTEXT: &str = "no extra tool context";

#[derive(Debug, Default, PartialEq, Eq)]
struct ToolSetCache {
    schemas_json: Option<String>,
    tool_context: Option<String>,
    extra_tool_context: Option<String>,
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
enum ToolSource {
    Registry,
    Local,
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
enum ToolState {
    Enabled,
    Disabled,
    TemporarilyEnabled,
    TemporarilyDisabled,
}

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum ToolSetError {
    #[error("tool already exists: {0}")]
    AlreadyExists(ToolName),
    #[error("tool not found: {0}")]
    NotFound(ToolName),
    #[error("tool is not local: {0}")]
    NotLocal(ToolName),
}

pub struct ToolSet {
    registry: Arc<ToolRegistry>,
    tools: HashMap<ToolName, Tool>,
    state: DurableState<ToolSetState>,
    cache: ToolSetCache,
    should_rebuild_temporary_tool: bool,
    should_rebuild_tool: bool,
}

#[derive(Default, Deserialize, Serialize)]
struct ToolSetState {
    registry_version: ToolRegistryVersion,
    tools: BTreeMap<ToolName, ToolSetEntryState>,
}

#[derive(Clone, Copy, Debug, Deserialize, PartialEq, Eq, Serialize)]
struct ToolSetEntryState {
    source: ToolSource,
    state: ToolState,
}

impl DurableStateCodec for ToolSetState {
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

impl ToolSet {
    pub(super) fn new(registry: Arc<ToolRegistry>) -> Self {
        Self {
            registry,
            tools: HashMap::new(),
            state: DurableState::default(),
            cache: ToolSetCache::default(),
            should_rebuild_temporary_tool: false,
            should_rebuild_tool: false,
        }
    }

    pub fn add_tool(&mut self, tool: Tool) -> Result<(), ToolSetError> {
        let name = tool.name().to_owned();
        if self.state.get().tools.contains_key(&name) || self.registry.contains_tool(&name) {
            return Err(ToolSetError::AlreadyExists(name));
        }
        self.tools.insert(name.clone(), tool);
        self.state.get_mut().tools.insert(
            name,
            ToolSetEntryState {
                source: ToolSource::Local,
                state: ToolState::Enabled,
            },
        );
        self.should_rebuild_tool = true;
        Ok(())
    }

    pub fn remove_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        match self.state.get().tools.get(&name) {
            Some(entry) if entry.source == ToolSource::Local => {}
            Some(_) => return Err(ToolSetError::NotLocal(name)),
            None => return Err(ToolSetError::NotFound(name)),
        }
        self.tools.remove(&name);
        self.state.get_mut().tools.remove(&name);
        self.should_rebuild_tool = true;
        Ok(())
    }

    pub fn enable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some(entry) = self.state.get().tools.get(&name).copied() else {
            return Err(ToolSetError::NotFound(name));
        };
        let changed = entry.state != ToolState::Enabled;
        match entry.state {
            ToolState::Enabled => {}
            ToolState::Disabled => self.should_rebuild_tool = true,
            ToolState::TemporarilyEnabled | ToolState::TemporarilyDisabled => {
                self.should_rebuild_tool = true;
                self.should_rebuild_temporary_tool = true;
            }
        }
        if changed {
            if let Some(entry) = self.state.get_mut().tools.get_mut(&name) {
                entry.state = ToolState::Enabled;
            }
        }
        Ok(())
    }

    pub fn disable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some(entry) = self.state.get().tools.get(&name).copied() else {
            return Err(ToolSetError::NotFound(name));
        };
        let changed = entry.state != ToolState::Disabled;
        match entry.state {
            ToolState::Disabled => {}
            ToolState::Enabled => self.should_rebuild_tool = true,
            ToolState::TemporarilyEnabled | ToolState::TemporarilyDisabled => {
                self.should_rebuild_tool = true;
                self.should_rebuild_temporary_tool = true;
            }
        }
        if changed {
            if let Some(entry) = self.state.get_mut().tools.get_mut(&name) {
                entry.state = ToolState::Disabled;
            }
        }
        Ok(())
    }

    pub fn temporarily_enable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some(entry) = self.state.get().tools.get(&name).copied() else {
            return Err(ToolSetError::NotFound(name));
        };
        let next = match entry.state {
            ToolState::Disabled => {
                self.should_rebuild_temporary_tool = true;
                Some(ToolState::TemporarilyEnabled)
            }
            ToolState::TemporarilyDisabled => {
                self.should_rebuild_temporary_tool = true;
                Some(ToolState::Enabled)
            }
            ToolState::Enabled | ToolState::TemporarilyEnabled => None,
        };
        if let Some(next) = next {
            if let Some(entry) = self.state.get_mut().tools.get_mut(&name) {
                entry.state = next;
            }
        }
        Ok(())
    }

    pub fn temporarily_disable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some(entry) = self.state.get().tools.get(&name).copied() else {
            return Err(ToolSetError::NotFound(name));
        };
        let next = match entry.state {
            ToolState::Enabled => {
                self.should_rebuild_temporary_tool = true;
                Some(ToolState::TemporarilyDisabled)
            }
            ToolState::TemporarilyEnabled => {
                self.should_rebuild_temporary_tool = true;
                Some(ToolState::Disabled)
            }
            ToolState::Disabled | ToolState::TemporarilyDisabled => None,
        };
        if let Some(next) = next {
            if let Some(entry) = self.state.get_mut().tools.get_mut(&name) {
                entry.state = next;
            }
        }
        Ok(())
    }

    pub fn clear_temporary_tools(&mut self) {
        let changes: Vec<_> = self
            .state
            .get()
            .tools
            .iter()
            .filter_map(|(name, entry)| match entry.state {
                ToolState::TemporarilyEnabled => Some((name.clone(), ToolState::Disabled)),
                ToolState::TemporarilyDisabled => Some((name.clone(), ToolState::Enabled)),
                ToolState::Enabled | ToolState::Disabled => None,
            })
            .collect();
        if changes.is_empty() {
            return;
        }
        let state = self.state.get_mut();
        for (name, next) in changes {
            if let Some(entry) = state.tools.get_mut(&name) {
                entry.state = next;
            }
        }
        self.should_rebuild_temporary_tool = true;
    }

    pub fn restore_state(&mut self, state: PartStateSlice<'_>) -> Result<(), DurablePartError> {
        let mut restored = ToolSetState::decode_state(state)?;
        restored.tools.retain(|name, entry| {
            entry.source == ToolSource::Registry || self.tools.contains_key(name)
        });
        self.state = DurableState::new(restored);
        self.cache = ToolSetCache::default();
        self.should_rebuild_temporary_tool = true;
        self.should_rebuild_tool = true;
        Ok(())
    }

    pub fn begin(&mut self) -> Result<ToolSetHandle<'_>, ToolSetError> {
        let registry_version = self.registry.tool_version();
        if self.state.get().registry_version != registry_version {
            self.rebuild();
        } else if self.should_rebuild_tool {
            self.rebuild_cache();
        } else if self.should_rebuild_temporary_tool {
            self.rebuild_extra_tool_context();
        }
        Ok(ToolSetHandle {
            tools: &self.tools,
            states: &self.state.get().tools,
            cache: &self.cache,
        })
    }

    fn rebuild(&mut self) {
        let projection = self.registry.tool_projection();
        let registry_names = projection
            .tools
            .iter()
            .map(|entry| entry.name.clone())
            .collect::<HashSet<_>>();

        self.tools.retain(|name, _| {
            self.state
                .get()
                .tools
                .get(name)
                .is_some_and(|entry| entry.source == ToolSource::Local)
                || registry_names.contains(name)
        });

        let mut tool_states = self.state.get().tools.clone();
        tool_states.retain(|name, entry| {
            entry.source == ToolSource::Local || registry_names.contains(name)
        });

        for entry in projection.tools {
            let state =
                tool_states
                    .get(&entry.name)
                    .and_then(|tool_state| match tool_state.source {
                        ToolSource::Registry => Some(tool_state.state),
                        ToolSource::Local => {
                            tracing::trace!(
                                tool = entry.name.as_str(),
                                "registry tool overrides local tool"
                            );
                            None
                        }
                    });
            let state = match state {
                Some(state) => state,
                None => ToolState::Enabled,
            };
            self.tools.insert(entry.name.clone(), entry.tool);
            tool_states.insert(
                entry.name,
                ToolSetEntryState {
                    source: ToolSource::Registry,
                    state,
                },
            );
        }

        if self.state.get().registry_version != projection.registry_version
            || self.state.get().tools != tool_states
        {
            self.state.replace(ToolSetState {
                registry_version: projection.registry_version,
                tools: tool_states,
            });
        }
        self.rebuild_cache();
    }

    fn rebuild_cache(&mut self) {
        self.render_schemas_json();
        self.render_tool_context();
        self.rebuild_extra_tool_context();
        self.should_rebuild_tool = false;
    }

    fn rebuild_extra_tool_context(&mut self) {
        self.render_extra_tool_context();
        self.should_rebuild_temporary_tool = false;
    }

    fn render_schemas_json(&mut self) {
        let schemas_json = self.cache.schemas_json.get_or_insert_with(String::new);
        schemas_json.clear();

        let mut has_tool = false;
        schemas_json.push('[');
        for (name, entry) in &self.state.get().tools {
            if !matches!(
                entry.state,
                ToolState::Enabled | ToolState::TemporarilyDisabled
            ) {
                continue;
            }
            let Some(tool) = self.tools.get(name) else {
                continue;
            };
            if has_tool {
                schemas_json.push(',');
            }
            schemas_json.push_str(tool.schema());
            has_tool = true;
        }
        if has_tool {
            schemas_json.push(']');
        } else {
            schemas_json.clear();
        }
    }

    fn render_tool_context(&mut self) {
        let tool_context = self.cache.tool_context.get_or_insert_with(String::new);
        tool_context.clear();

        for (name, entry) in &self.state.get().tools {
            if !matches!(
                entry.state,
                ToolState::Enabled | ToolState::TemporarilyDisabled
            ) {
                continue;
            }
            let Some(tool) = self.tools.get(name) else {
                continue;
            };
            let Some(usage) = tool.usage() else {
                continue;
            };
            if !tool_context.is_empty() {
                tool_context.push_str("\n\n");
            }
            tool_context.push_str(usage);
        }
    }

    fn render_extra_tool_context(&mut self) {
        let extra_context = self
            .cache
            .extra_tool_context
            .get_or_insert_with(String::new);
        extra_context.clear();

        for (name, entry) in &self.state.get().tools {
            match entry.state {
                ToolState::TemporarilyEnabled => {
                    let Some(tool) = self.tools.get(name) else {
                        continue;
                    };
                    if !extra_context.is_empty() {
                        extra_context.push_str("\n\n");
                    }
                    extra_context.push_str("Tool `");
                    extra_context.push_str(name);
                    extra_context.push_str("` is temporarily available.\n");
                    match tool.usage() {
                        Some(usage) => extra_context.push_str(usage),
                        None => extra_context.push_str(tool.schema()),
                    }
                }
                ToolState::TemporarilyDisabled => {
                    if !extra_context.is_empty() {
                        extra_context.push_str("\n\n");
                    }
                    extra_context.push_str("Tool `");
                    extra_context.push_str(name);
                    extra_context.push_str("` is temporarily unavailable.");
                }
                ToolState::Enabled | ToolState::Disabled => {}
            }
        }
    }
}

impl DurablePart for ToolSet {
    fn name(&self) -> &'static str {
        "tool-set"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}

pub struct ToolSetHandle<'a> {
    tools: &'a HashMap<ToolName, Tool>,
    states: &'a BTreeMap<ToolName, ToolSetEntryState>,
    cache: &'a ToolSetCache,
}

impl<'a> ToolSetHandle<'a> {
    pub fn schemas_json(&self) -> &str {
        match self
            .cache
            .schemas_json
            .as_deref()
            .filter(|text| !text.is_empty())
        {
            Some(schemas_json) => schemas_json,
            None => NO_SCHEMAS,
        }
    }

    pub fn tool_context(&self) -> &str {
        match self
            .cache
            .tool_context
            .as_deref()
            .filter(|text| !text.is_empty())
        {
            Some(tool_context) => tool_context,
            None => NO_TOOL_CONTEXT,
        }
    }

    pub fn extra_tool_context(&self) -> &str {
        match self
            .cache
            .extra_tool_context
            .as_deref()
            .filter(|text| !text.is_empty())
        {
            Some(extra_tool_context) => extra_tool_context,
            None => NO_EXTRA_TOOL_CONTEXT,
        }
    }

    pub(crate) fn classify(&self, call: &ToolInvocation<'_>) -> ToolResult<Action> {
        match (self.tools.get(call.name()), self.states.get(call.name())) {
            (Some(tool), Some(entry))
                if matches!(
                    entry.state,
                    ToolState::Enabled | ToolState::TemporarilyEnabled
                ) =>
            {
                Ok(tool.classify(call))
            }
            (_, Some(entry)) if entry.state == ToolState::TemporarilyDisabled => {
                Err(ToolError::InvokeRejected(unavailable_message(call.name())).into())
            }
            _ => Err(ToolError::NotFound(call.name().to_owned()).into()),
        }
    }

    pub async fn invoke<'call>(
        &self,
        call: &'call ToolInvocation<'call>,
    ) -> ToolResult<ToolOutput> {
        match (self.tools.get(call.name()), self.states.get(call.name())) {
            (Some(tool), Some(entry))
                if matches!(
                    entry.state,
                    ToolState::Enabled | ToolState::TemporarilyEnabled
                ) =>
            {
                tool.invoke(call).await
            }
            (_, Some(entry)) if entry.state == ToolState::TemporarilyDisabled => {
                Err(ToolError::InvokeRejected(unavailable_message(call.name())).into())
            }
            _ => Err(ToolError::NotFound(call.name().to_owned()).into()),
        }
    }
}

fn unavailable_message(name: &str) -> String {
    let mut message = String::from("tool is temporarily unavailable: ");
    message.push_str(name);
    message
}
