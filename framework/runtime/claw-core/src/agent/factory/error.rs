use claw_memory::{LongTermInitError, TranscriptInitError};
use claw_skill::SkillError;
use claw_tool::ToolSetError;

use crate::agent::base_agent::{AgentCommandError, BaseAgentBuildError};
use crate::agent::config::AgentConfigError;
use crate::agent::generic_agent::GenericAgentBuildError;

/// What can go wrong while building an [`super::FsAgentFactory`].
#[derive(Debug, thiserror::Error)]
pub enum FsAgentFactoryError {
    /// No persistence directory was provided to the factory.
    #[error("persistence directory is required")]
    MissingPersistenceDir,
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(#[from] LongTermInitError),
    /// The configured skill catalog could not be scanned.
    #[error("failed to load skill catalog: {0}")]
    SkillRegistry(#[from] SkillError),
}

/// What can go wrong while building one concrete agent from the factory.
#[derive(Debug, thiserror::Error)]
pub(crate) enum FsAgentCreateError {
    /// The baked manifest could not be resolved into an agent config.
    #[error("failed to resolve agent config: {0}")]
    Config(#[from] AgentConfigError),
    /// The agent's local tools could not be added to the tool set.
    #[error("failed to assemble agent tools: {0}")]
    Tools(#[from] ToolSetError),
    /// The transcript store for this placement could not be opened.
    #[error("failed to open transcript: {0}")]
    Transcript(#[from] TranscriptInitError),
    /// The generic agent failed to build.
    #[error("failed to build agent: {0}")]
    Agent(#[from] GenericAgentBuildError),
    /// The profile context adapter could not be attached.
    #[error("failed to attach profile context: {0}")]
    ProfileContext(#[source] BaseAgentBuildError),
    /// The per-agent long-term memory store could not be opened.
    #[error("failed to load long-term memory: {0}")]
    LongTerm(#[from] LongTermInitError),
    /// The long-term memory context adapter could not be attached.
    #[error("failed to attach long-term memory context: {0}")]
    LongTermContext(#[source] BaseAgentBuildError),
    /// The initial goal could not be enqueued.
    #[error("failed to seed initial goal: {0}")]
    Goal(#[from] AgentCommandError),
}
