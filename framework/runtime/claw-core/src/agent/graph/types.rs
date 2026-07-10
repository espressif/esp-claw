use serde::ser::{SerializeStruct, Serializer};
use serde::Serialize;
use strum::{EnumString, IntoStaticStr};

use crate::agent::base_agent::AgentId;
use crate::agent::kind::AgentKind;

#[derive(Clone, Copy, Debug, Default, EnumString, IntoStaticStr, PartialEq, Eq)]
#[strum(
    parse_err_ty = ParseTerminationPolicyError,
    parse_err_fn = ParseTerminationPolicyError::new
)]
pub(crate) enum TerminationPolicy {
    #[default]
    #[strum(serialize = "auto")]
    AutoOnIdle,
    #[strum(serialize = "manual")]
    Manual,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, thiserror::Error)]
#[error("unknown termination policy; expected auto or manual")]
pub(crate) struct ParseTerminationPolicyError;

impl ParseTerminationPolicyError {
    fn new(_: &str) -> Self {
        Self
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum GraphEffect {
    Spawn {
        id: AgentId,
        kind: AgentKind,
        name: Option<String>,
        goal: String,
        termination: TerminationPolicy,
    },
    Delete {
        target: AgentId,
    },
    Followup {
        target: AgentId,
        message: String,
    },
}

#[derive(Clone, Copy, Debug, IntoStaticStr, PartialEq, Eq)]
pub(crate) enum AgentStatus {
    #[strum(serialize = "ready")]
    Ready,
    #[strum(serialize = "awaiting_approval")]
    AwaitingApproval,
    #[strum(serialize = "idle")]
    Idle,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct AgentSnapshot {
    pub id: AgentId,
    pub kind: AgentKind,
    pub name: Option<String>,
    pub parent: Option<AgentId>,
    pub depth: u16,
    pub termination: TerminationPolicy,
    pub status: AgentStatus,
}

impl Serialize for AgentSnapshot {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        let mut state = serializer.serialize_struct("AgentSnapshot", 7)?;
        state.serialize_field("agent", &self.id)?;
        state.serialize_field("kind", self.kind.as_str())?;
        state.serialize_field("name", &self.name)?;
        state.serialize_field("parent", &self.parent)?;
        state.serialize_field("depth", &self.depth)?;
        let status: &'static str = self.status.into();
        state.serialize_field("status", status)?;
        let termination: &'static str = self.termination.into();
        state.serialize_field("termination", termination)?;
        state.end()
    }
}
