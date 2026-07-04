use std::fmt;
use std::sync::Arc;

use crate::tool::{Tool, ToolRegistry, ToolRegistryError, ToolSet};

pub type CapabilityId = String;
pub type CapabilityResult<T> = Result<T, CapabilityRegistryError>;

#[derive(Clone, Debug)]
pub enum Capability {
    Tool(Tool),
}

impl Capability {
    pub fn from_tool(tool: Tool) -> Self {
        Self::Tool(tool)
    }

    pub fn id(&self) -> &str {
        match self {
            Self::Tool(tool) => tool.name(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum CapabilityRegistryError {
    #[error(transparent)]
    Tool(#[from] ToolRegistryError),
}

#[derive(Default)]
pub struct CapabilityRegistry {
    tools: Arc<ToolRegistry>,
}

impl CapabilityRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register(&self, capability: Capability) -> CapabilityResult<()> {
        match capability {
            Capability::Tool(tool) => self.tools.register(tool)?,
        }
        Ok(())
    }

    pub fn enable(&self, capability_id: &str) -> CapabilityResult<()> {
        self.tools.enable(capability_id)?;
        Ok(())
    }

    pub fn disable(&self, capability_id: &str) -> CapabilityResult<()> {
        self.tools.disable(capability_id)?;
        Ok(())
    }

    pub fn start_all(&self) -> CapabilityResult<()> {
        self.tools.start_all()?;
        Ok(())
    }

    pub fn stop_all(&self) -> CapabilityResult<()> {
        self.tools.stop_all()?;
        Ok(())
    }

    pub fn tool_set(&self) -> ToolSet {
        self.tools.tool_set()
    }
}

impl fmt::Debug for CapabilityRegistry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CapabilityRegistry")
            .field("tools", &self.tools)
            .finish()
    }
}
