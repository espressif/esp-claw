use crate::agent::{AgentCommandError, AgentId, FsAgentCreateError};

#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum ApprovalResolutionError {
    #[error("no active approval to resolve")]
    NoActiveApproval,
    #[error("no agent {0} to resolve approval for")]
    UnknownAgent(AgentId),
    #[error(transparent)]
    Command(AgentCommandError),
}

#[derive(Debug, thiserror::Error)]
pub(crate) enum InstanceDeliverError {
    #[error("failed to build root agent: {0}")]
    Create(#[from] FsAgentCreateError),
    #[error("failed to deliver to root {root}: {source}")]
    Root {
        root: AgentId,
        #[source]
        source: AgentMessageDeliveryError,
    },
}

#[derive(Clone, Debug, thiserror::Error)]
pub(crate) enum AgentMessageDeliveryError {
    #[error("no such agent: {0}")]
    UnknownAgent(AgentId),
    #[error(transparent)]
    Command(#[from] AgentCommandError),
}
