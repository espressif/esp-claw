use std::fmt;
use std::sync::Arc;

use crate::channel::{Channel, ChannelRegistry, ChannelRegistryError, ChannelSink};
use crate::tool::{Tool, ToolRegistry, ToolRegistryError, ToolSet};

pub type CapabilityId = String;
pub type CapabilityResult<T> = Result<T, CapabilityRegistryError>;

#[derive(Clone, Debug)]
pub enum Capability {
    Tool(Tool),
    Channel(Channel),
}

impl Capability {
    pub fn from_tool(tool: Tool) -> Self {
        Self::Tool(tool)
    }

    pub fn from_channel(channel: Channel) -> Self {
        Self::Channel(channel)
    }

    pub fn id(&self) -> &str {
        match self {
            Self::Tool(tool) => tool.name(),
            Self::Channel(channel) => channel.name(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum CapabilityRegistryError {
    #[error(transparent)]
    Tool(#[from] ToolRegistryError),
    #[error(transparent)]
    Channel(#[from] ChannelRegistryError),
}

#[derive(Default)]
pub struct CapabilityRegistry {
    tools: Arc<ToolRegistry>,
    channels: ChannelRegistry,
}

impl CapabilityRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn with_channel_sink(sink: ChannelSink) -> Self {
        Self {
            tools: Arc::new(ToolRegistry::new()),
            channels: ChannelRegistry::new(sink),
        }
    }

    pub fn register(&self, capability: Capability) -> CapabilityResult<()> {
        match capability {
            Capability::Tool(tool) => self.tools.register(tool)?,
            Capability::Channel(channel) => self.channels.register(channel)?,
        }
        Ok(())
    }

    pub fn start_all(&self) -> CapabilityResult<()> {
        self.tools.start_all()?;
        self.channels.start_all()?;
        Ok(())
    }

    pub fn stop_all(&self) -> CapabilityResult<()> {
        self.channels.stop_all()?;
        self.tools.stop_all()?;
        Ok(())
    }

    pub fn tool_set(&self) -> ToolSet {
        self.tools.tool_set()
    }

    pub fn channel_registry(&self) -> &ChannelRegistry {
        &self.channels
    }
}

impl fmt::Debug for CapabilityRegistry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("CapabilityRegistry")
            .field("tools", &self.tools)
            .field("channels", &self.channels)
            .finish()
    }
}
