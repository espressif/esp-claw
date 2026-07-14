use claw_context::Block;
use claw_tool::ToolGroup;

/// Transcript identity and storage for one independently-built agent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TranscriptTarget {
    InMemory(u32),
    Persistent(u32),
}

impl TranscriptTarget {
    pub(super) fn id(self) -> u32 {
        match self {
            Self::InMemory(id) | Self::Persistent(id) => id,
        }
    }

    pub(super) fn persists(self) -> bool {
        matches!(self, Self::Persistent(_))
    }
}

/// Whether this agent may mutate the shared profile exposed in its context.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ProfileAccess {
    ReadOnly,
    Writable,
}

/// Runtime capabilities supplied by the owner constructing one agent.
///
/// The agent factory does not interpret its owner's orchestration model.
/// Extension tools are ordinary tool groups by the time they cross this
/// boundary.
pub(crate) struct AgentEnvironment {
    pub(super) transcript: TranscriptTarget,
    pub(super) profile: ProfileAccess,
    pub(super) extension_tools: Vec<ToolGroup>,
    pub(super) inherited_context: Vec<Block<'static>>,
}

impl AgentEnvironment {
    pub(crate) fn new(
        transcript: TranscriptTarget,
        profile: ProfileAccess,
        extension_tools: Vec<ToolGroup>,
        inherited_context: Vec<Block<'static>>,
    ) -> Self {
        Self {
            transcript,
            profile,
            extension_tools,
            inherited_context,
        }
    }
}
