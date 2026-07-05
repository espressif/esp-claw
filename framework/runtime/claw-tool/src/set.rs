use std::collections::{HashMap, HashSet};
use std::sync::Arc;

use claw_permission::Action;

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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ToolSource {
    Registry,
    Local,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ToolState {
    Enabled,
    Disabled,
    TemporailyEnabled,
    TemporailyDisabled,
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
    tools: HashMap<ToolName, (Tool, ToolSource, ToolState)>,
    cache: ToolSetCache,
    registry_version: ToolRegistryVersion,
    should_rebuild_temporary_tool: bool,
    should_rebuild_tool: bool,
}

impl ToolSet {
    pub(super) fn new(registry: Arc<ToolRegistry>) -> Self {
        Self {
            registry,
            tools: HashMap::new(),
            cache: ToolSetCache::default(),
            registry_version: 0,
            should_rebuild_temporary_tool: false,
            should_rebuild_tool: false,
        }
    }

    pub fn add_tool(&mut self, tool: Tool) -> Result<(), ToolSetError> {
        let name = tool.name().to_owned();
        if self.tools.contains_key(&name) || self.registry.contains_tool(&name) {
            return Err(ToolSetError::AlreadyExists(name));
        }
        self.tools
            .insert(name, (tool, ToolSource::Local, ToolState::Enabled));
        self.should_rebuild_tool = true;
        Ok(())
    }

    pub fn remove_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        match self.tools.get(&name) {
            Some((_, ToolSource::Local, _)) => {}
            Some((_, ToolSource::Registry, _)) => return Err(ToolSetError::NotLocal(name)),
            None => return Err(ToolSetError::NotFound(name)),
        }
        self.tools.remove(&name);
        self.should_rebuild_tool = true;
        Ok(())
    }

    pub fn enable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some((_, _, state)) = self.tools.get_mut(&name) else {
            return Err(ToolSetError::NotFound(name));
        };
        match state {
            ToolState::Enabled => {}
            ToolState::Disabled => self.should_rebuild_tool = true,
            ToolState::TemporailyEnabled | ToolState::TemporailyDisabled => {
                self.should_rebuild_tool = true;
                self.should_rebuild_temporary_tool = true;
            }
        }
        *state = ToolState::Enabled;
        Ok(())
    }

    pub fn disable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some((_, _, state)) = self.tools.get_mut(&name) else {
            return Err(ToolSetError::NotFound(name));
        };
        match state {
            ToolState::Disabled => {}
            ToolState::Enabled => self.should_rebuild_tool = true,
            ToolState::TemporailyEnabled | ToolState::TemporailyDisabled => {
                self.should_rebuild_tool = true;
                self.should_rebuild_temporary_tool = true;
            }
        }
        *state = ToolState::Disabled;
        Ok(())
    }

    pub fn temporarily_enable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some((_, _, state)) = self.tools.get_mut(&name) else {
            return Err(ToolSetError::NotFound(name));
        };
        match state {
            ToolState::Disabled => {
                *state = ToolState::TemporailyEnabled;
                self.should_rebuild_temporary_tool = true;
            }
            ToolState::TemporailyDisabled => {
                *state = ToolState::Enabled;
                self.should_rebuild_temporary_tool = true;
            }
            ToolState::Enabled | ToolState::TemporailyEnabled => {}
        }
        Ok(())
    }

    pub fn temporarily_disable_tool(&mut self, name: ToolName) -> Result<(), ToolSetError> {
        let Some((_, _, state)) = self.tools.get_mut(&name) else {
            return Err(ToolSetError::NotFound(name));
        };
        match state {
            ToolState::Enabled => {
                *state = ToolState::TemporailyDisabled;
                self.should_rebuild_temporary_tool = true;
            }
            ToolState::TemporailyEnabled => {
                *state = ToolState::Disabled;
                self.should_rebuild_temporary_tool = true;
            }
            ToolState::Disabled | ToolState::TemporailyDisabled => {}
        }
        Ok(())
    }

    pub fn clear_temporary_tools(&mut self) {
        for (_, _, state) in self.tools.values_mut() {
            match state {
                ToolState::TemporailyEnabled => {
                    *state = ToolState::Disabled;
                    self.should_rebuild_temporary_tool = true;
                }
                ToolState::TemporailyDisabled => {
                    *state = ToolState::Enabled;
                    self.should_rebuild_temporary_tool = true;
                }
                ToolState::Enabled | ToolState::Disabled => {}
            }
        }
    }

    pub fn begin(&mut self) -> Result<ToolSetHandle<'_>, ToolSetError> {
        let registry_version = self.registry.tool_version();
        if self.registry_version != registry_version {
            self.rebuild();
        } else if self.should_rebuild_tool {
            self.rebuild_cache();
        } else if self.should_rebuild_temporary_tool {
            self.rebuild_extra_tool_context();
        }
        Ok(ToolSetHandle {
            tools: &self.tools,
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

        self.tools.retain(|name, (_, source, _)| {
            *source == ToolSource::Local || registry_names.contains(name)
        });

        for entry in projection.tools {
            let state = self
                .tools
                .get(&entry.name)
                .and_then(|(_, source, state)| match source {
                    ToolSource::Registry => Some(*state),
                    ToolSource::Local => {
                        tracing::trace!(
                            tool = entry.name.as_str(),
                            "registry tool overrides local tool"
                        );
                        None
                    }
                })
                .unwrap_or(ToolState::Enabled);
            self.tools
                .insert(entry.name, (entry.tool, ToolSource::Registry, state));
        }

        self.registry_version = projection.registry_version;
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
        for (tool, _, state) in self.tools.values() {
            if !matches!(state, ToolState::Enabled | ToolState::TemporailyDisabled) {
                continue;
            }
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

        for (tool, _, state) in self.tools.values() {
            if !matches!(state, ToolState::Enabled | ToolState::TemporailyDisabled) {
                continue;
            }
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

        for (name, (tool, _, state)) in &self.tools {
            match state {
                ToolState::TemporailyEnabled => {
                    if !extra_context.is_empty() {
                        extra_context.push_str("\n\n");
                    }
                    extra_context.push_str("Tool `");
                    extra_context.push_str(name);
                    extra_context.push_str("` is temporarily available.\n");
                    extra_context.push_str(tool.usage().unwrap_or(tool.schema()));
                }
                ToolState::TemporailyDisabled => {
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

pub struct ToolSetHandle<'a> {
    tools: &'a HashMap<ToolName, (Tool, ToolSource, ToolState)>,
    cache: &'a ToolSetCache,
}

impl<'a> ToolSetHandle<'a> {
    pub fn schemas_json(&self) -> &str {
        self.cache
            .schemas_json
            .as_deref()
            .filter(|text| !text.is_empty())
            .unwrap_or(NO_SCHEMAS)
    }

    pub fn tool_context(&self) -> &str {
        self.cache
            .tool_context
            .as_deref()
            .filter(|text| !text.is_empty())
            .unwrap_or(NO_TOOL_CONTEXT)
    }

    pub fn extra_tool_context(&self) -> &str {
        self.cache
            .extra_tool_context
            .as_deref()
            .filter(|text| !text.is_empty())
            .unwrap_or(NO_EXTRA_TOOL_CONTEXT)
    }

    pub(crate) fn classify(&self, call: &ToolInvocation<'_>) -> ToolResult<Action> {
        match self.tools.get(call.name()) {
            Some((tool, _, ToolState::Enabled | ToolState::TemporailyEnabled)) => {
                Ok(tool.classify(call))
            }
            Some((_, _, ToolState::TemporailyDisabled)) => {
                Err(ToolError::InvokeRejected(unavailable_message(call.name())).into())
            }
            Some((_, _, ToolState::Disabled)) | None => {
                Err(ToolError::NotFound(call.name().to_owned()).into())
            }
        }
    }

    pub async fn invoke<'call>(
        &self,
        call: &'call ToolInvocation<'call>,
    ) -> ToolResult<ToolOutput> {
        match self.tools.get(call.name()) {
            Some((tool, _, ToolState::Enabled | ToolState::TemporailyEnabled)) => {
                tool.invoke(call).await
            }
            Some((_, _, ToolState::TemporailyDisabled)) => {
                Err(ToolError::InvokeRejected(unavailable_message(call.name())).into())
            }
            Some((_, _, ToolState::Disabled)) | None => {
                Err(ToolError::NotFound(call.name().to_owned()).into())
            }
        }
    }
}

fn unavailable_message(name: &str) -> String {
    let mut message = String::from("tool is temporarily unavailable: ");
    message.push_str(name);
    message
}
