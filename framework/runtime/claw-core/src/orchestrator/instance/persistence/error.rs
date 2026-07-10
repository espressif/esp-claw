use std::error::Error;
use std::fmt;

use claw_checkpoint::DurablePartError;

use crate::agent::{AgentId, FsAgentCreateError};

#[derive(Debug)]
pub struct OrchestratorInstanceRestoreError {
    kind: OrchestratorInstanceRestoreErrorKind,
}

#[derive(Debug, thiserror::Error)]
enum OrchestratorInstanceRestoreErrorKind {
    #[error("failed to rebuild checkpointed agent {agent}: {source}")]
    Agent {
        agent: AgentId,
        #[source]
        source: FsAgentCreateError,
    },
    #[error("checkpointed agent is missing after rebuild: {0}")]
    MissingAgent(AgentId),
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

impl OrchestratorInstanceRestoreError {
    pub(in crate::orchestrator::instance) fn agent(
        agent: AgentId,
        source: FsAgentCreateError,
    ) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::Agent { agent, source },
        }
    }

    pub(in crate::orchestrator::instance) fn missing_agent(agent: AgentId) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::MissingAgent(agent),
        }
    }

    pub(in crate::orchestrator::instance) fn unknown_part(agent: AgentId, part: String) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::UnknownPart { agent, part },
        }
    }

    pub(in crate::orchestrator::instance) fn durable_part(
        agent: AgentId,
        part: String,
        source: DurablePartError,
    ) -> Self {
        Self {
            kind: OrchestratorInstanceRestoreErrorKind::DurablePart {
                agent,
                part,
                source,
            },
        }
    }
}

impl fmt::Display for OrchestratorInstanceRestoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.kind.fmt(formatter)
    }
}

impl Error for OrchestratorInstanceRestoreError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        self.kind.source()
    }
}
