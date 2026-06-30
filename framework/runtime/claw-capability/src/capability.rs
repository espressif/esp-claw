//! The external "capability" vocabulary: a slim descriptor that decomposes into
//! an internal *role* (Tool / Channel / none) plus an orthogonal *lifecycle*.

use std::sync::Arc;

use claw_tool::Tool;

use crate::channel::ChannelAdapter;
use crate::lifecycle::Lifecycle;

/// What a capability exposes when it is *used*. Orthogonal to its
/// [`Lifecycle`](crate::Lifecycle): any role may also own resources, and
/// [`None`](CapabilityRole::None) is a capability that exists *only* for its
/// lifecycle.
#[derive(Clone)]
pub enum CapabilityRole {
    /// A model-callable tool. A capability with this role *is* a
    /// [`claw_tool::Tool`]; this crate adds no dispatch, schema, or visibility
    /// logic of its own — `claw-core` composes these into per-agent `ToolSet`s.
    Tool(Tool),
    /// A message channel: outbound egress (and inbound via its lifecycle task).
    Channel(Arc<dyn ChannelAdapter>),
    /// No invocation surface; the capability exists only for its
    /// [`lifecycle`](Capability::lifecycle) (e.g. an MCP server managed purely
    /// by enable/disable).
    None,
}

/// One registered capability: identity, an optional description, a role, and an
/// optional lifecycle.
#[derive(Clone)]
pub struct Capability {
    /// Stable unique id.
    pub id: String,
    /// Human/model-readable description (catalog + tool prompt).
    pub description: Option<String>,
    /// What this capability exposes when used.
    pub role: CapabilityRole,
    /// Optional per-capability resource lifecycle (a gateway's transport task,
    /// an MCP server). Shared group resources go on [`CapabilityGroup::lifecycle`].
    pub lifecycle: Option<Arc<dyn Lifecycle>>,
}

impl Capability {
    /// A capability with the given id and role, no description, no lifecycle.
    pub fn new(id: impl Into<String>, role: CapabilityRole) -> Self {
        Self {
            id: id.into(),
            description: None,
            role,
            lifecycle: None,
        }
    }

    /// A [`Tool`](CapabilityRole::Tool) capability whose id is the tool's name.
    pub fn tool(tool: Tool) -> Self {
        Self::new(tool.name().to_string(), CapabilityRole::Tool(tool))
    }

    /// A [`Channel`](CapabilityRole::Channel) capability whose id is the channel id.
    pub fn channel(adapter: Arc<dyn ChannelAdapter>) -> Self {
        Self::new(
            adapter.channel_id().to_string(),
            CapabilityRole::Channel(adapter),
        )
    }

    /// Sets the description.
    #[must_use]
    pub fn with_description(mut self, description: impl Into<String>) -> Self {
        self.description = Some(description.into());
        self
    }

    /// Attaches a per-capability lifecycle.
    #[must_use]
    pub fn with_lifecycle(mut self, lifecycle: Arc<dyn Lifecycle>) -> Self {
        self.lifecycle = Some(lifecycle);
        self
    }

    /// The [`Tool`](claw_tool::Tool) this capability exposes, or `None` for the
    /// `Channel`/`None` roles.
    pub fn as_tool(&self) -> Option<&Tool> {
        match &self.role {
            CapabilityRole::Tool(tool) => Some(tool),
            _ => None,
        }
    }

    /// The [`ChannelAdapter`] this capability exposes, or `None` otherwise.
    pub fn as_channel(&self) -> Option<&Arc<dyn ChannelAdapter>> {
        match &self.role {
            CapabilityRole::Channel(channel) => Some(channel),
            _ => None,
        }
    }
}

/// A registrable bundle of capabilities plus an optional shared lifecycle.
///
/// A group is the unit of enable/disable and the home for resources its members
/// *share* — e.g. a single scripting runtime backing several tools. Single
/// capabilities are registered as a one-member group via
/// [`Registry::register`](crate::Registry::register).
#[derive(Clone)]
pub struct CapabilityGroup {
    /// Group id: provenance label + the enable/disable handle.
    pub id: String,
    /// Member capabilities, validated and registered together.
    pub members: Vec<Capability>,
    /// Optional shared lifecycle, run around the members' own lifecycles.
    pub lifecycle: Option<Arc<dyn Lifecycle>>,
}

impl CapabilityGroup {
    /// A group with the given id and members, no shared lifecycle.
    pub fn new(id: impl Into<String>, members: impl IntoIterator<Item = Capability>) -> Self {
        Self {
            id: id.into(),
            members: members.into_iter().collect(),
            lifecycle: None,
        }
    }

    /// Attaches a shared group lifecycle.
    #[must_use]
    pub fn with_lifecycle(mut self, lifecycle: Arc<dyn Lifecycle>) -> Self {
        self.lifecycle = Some(lifecycle);
        self
    }
}
