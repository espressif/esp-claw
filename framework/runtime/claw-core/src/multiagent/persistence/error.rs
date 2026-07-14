use std::error::Error;
use std::fmt;

use claw_checkpoint::DurablePartError;

use crate::agent::FsAgentCreateError;
use crate::protocol::AgentId;

#[derive(Debug)]
pub(crate) struct MultiagentRestoreError {
    kind: MultiagentRestoreErrorKind,
}

#[derive(Debug, thiserror::Error)]
enum MultiagentRestoreErrorKind {
    #[error("failed to rebuild checkpointed agent {agent}: {source}")]
    Agent {
        agent: AgentId,
        #[source]
        source: FsAgentCreateError,
    },
    #[error("checkpointed agent is missing after rebuild: {0}")]
    MissingAgent(AgentId),
    #[error("checkpointed durable parts do not match the rebuilt agent: {0}")]
    PartRoster(AgentId),
    #[error("unknown checkpointed agent part {part} for {agent}")]
    UnknownPart { agent: AgentId, part: String },
    #[error("failed to restore checkpointed agent part {part} for {agent}: {source}")]
    DurablePart {
        agent: AgentId,
        part: String,
        #[source]
        source: DurablePartError,
    },
}

impl MultiagentRestoreError {
    pub(in crate::multiagent) fn agent(agent: AgentId, source: FsAgentCreateError) -> Self {
        Self {
            kind: MultiagentRestoreErrorKind::Agent { agent, source },
        }
    }

    pub(in crate::multiagent) fn missing_agent(agent: AgentId) -> Self {
        Self {
            kind: MultiagentRestoreErrorKind::MissingAgent(agent),
        }
    }

    pub(in crate::multiagent) fn part_roster(agent: AgentId) -> Self {
        Self {
            kind: MultiagentRestoreErrorKind::PartRoster(agent),
        }
    }

    pub(in crate::multiagent) fn unknown_part(agent: AgentId, part: String) -> Self {
        Self {
            kind: MultiagentRestoreErrorKind::UnknownPart { agent, part },
        }
    }

    pub(in crate::multiagent) fn durable_part(
        agent: AgentId,
        part: String,
        source: DurablePartError,
    ) -> Self {
        Self {
            kind: MultiagentRestoreErrorKind::DurablePart {
                agent,
                part,
                source,
            },
        }
    }
}

impl fmt::Display for MultiagentRestoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.kind.fmt(formatter)
    }
}

impl Error for MultiagentRestoreError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        self.kind.source()
    }
}
