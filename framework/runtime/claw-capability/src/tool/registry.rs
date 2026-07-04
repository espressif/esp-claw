use std::collections::HashMap;
use std::fmt;
use std::sync::{Arc, PoisonError, RwLock, RwLockReadGuard, RwLockWriteGuard};

use super::set::{ToolName, ToolSet};
use super::tool::Tool;

pub type RegistryVersion = u64;

#[derive(Default)]
pub struct ToolRegistry {
    inner: RwLock<ToolRegistryState>,
}

#[derive(Default)]
struct ToolRegistryState {
    tools: HashMap<ToolName, ToolRegistryEntry>,
    started: bool,
    version: RegistryVersion,
}

struct ToolRegistryEntry {
    tool: Tool,
    enabled: bool,
}

pub(super) struct ToolProjection {
    pub registry_version: RegistryVersion,
    pub tools: Vec<ToolProjectionEntry>,
}

pub(super) struct ToolProjectionEntry {
    pub name: ToolName,
    pub tool: Tool,
}

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum ToolRegistryError {
    #[error("tool already exists: {0}")]
    AlreadyExists(ToolName),
    #[error("tool not found: {0}")]
    NotFound(ToolName),
    #[error("invalid tool: {0}")]
    InvalidTool(ToolName),
}

impl ToolRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register(&self, tool: Tool) -> Result<(), ToolRegistryError> {
        let name = tool.name().to_owned();
        let mut state = self.write_state();
        if name.is_empty() {
            return Err(ToolRegistryError::InvalidTool(name));
        }
        if state.tools.contains_key(&name) {
            return Err(ToolRegistryError::AlreadyExists(name));
        }
        state.tools.insert(
            name,
            ToolRegistryEntry {
                tool,
                enabled: true,
            },
        );
        state.bump_version();
        Ok(())
    }

    pub fn enable(&self, name: &str) -> Result<(), ToolRegistryError> {
        let mut state = self.write_state();
        let Some(entry) = state.tools.get_mut(name) else {
            return Err(ToolRegistryError::NotFound(name.to_owned()));
        };
        if !entry.enabled {
            entry.enabled = true;
            state.bump_version();
        }
        Ok(())
    }

    pub fn disable(&self, name: &str) -> Result<(), ToolRegistryError> {
        let mut state = self.write_state();
        let Some(entry) = state.tools.get_mut(name) else {
            return Err(ToolRegistryError::NotFound(name.to_owned()));
        };
        if entry.enabled {
            entry.enabled = false;
            state.bump_version();
        }
        Ok(())
    }

    pub fn start_all(&self) -> Result<(), ToolRegistryError> {
        let mut state = self.write_state();
        if !state.started {
            state.started = true;
            state.bump_version();
        }
        Ok(())
    }

    pub fn stop_all(&self) -> Result<(), ToolRegistryError> {
        let mut state = self.write_state();
        if state.started {
            state.started = false;
            state.bump_version();
        }
        Ok(())
    }

    pub fn tool_set(self: &Arc<Self>) -> ToolSet {
        ToolSet::new(self.clone())
    }

    pub fn version(&self) -> RegistryVersion {
        self.read_state().version
    }

    pub(super) fn contains_tool(&self, name: &str) -> bool {
        self.read_state().tools.contains_key(name)
    }

    pub(super) fn tool_projection(&self) -> ToolProjection {
        let state = self.read_state();
        let mut tools = Vec::with_capacity(state.tools.len());
        for (name, entry) in &state.tools {
            if state.started && entry.enabled {
                tools.push(ToolProjectionEntry {
                    name: name.clone(),
                    tool: entry.tool.clone(),
                });
            }
        }
        ToolProjection {
            registry_version: state.version,
            tools,
        }
    }

    fn read_state(&self) -> RwLockReadGuard<'_, ToolRegistryState> {
        self.inner.read().unwrap_or_else(PoisonError::into_inner)
    }

    fn write_state(&self) -> RwLockWriteGuard<'_, ToolRegistryState> {
        self.inner.write().unwrap_or_else(PoisonError::into_inner)
    }
}

impl ToolRegistryState {
    fn bump_version(&mut self) {
        self.version = self.version.saturating_add(1);
    }
}

impl fmt::Debug for ToolRegistry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let state = self.read_state();
        formatter
            .debug_struct("ToolRegistry")
            .field("tools", &state.tools.len())
            .field("started", &state.started)
            .field("version", &state.version)
            .finish()
    }
}
