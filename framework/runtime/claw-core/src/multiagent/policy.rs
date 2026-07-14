use crate::config::catalog as agent_catalog;
use crate::protocol::AgentKind;

#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) enum SpawnPolicy {
    Any,
    Only(Vec<AgentKind>),
}

impl SpawnPolicy {
    pub(super) fn for_agent(kind: &AgentKind) -> Option<Self> {
        let manifest = agent_catalog::find(kind)?.multiagent();
        manifest
            .spawn_enabled()
            .then(|| Self::from_allowed_kinds(manifest.allowed_kinds()))
    }

    fn from_allowed_kinds(allowed_kinds: &[AgentKind]) -> Self {
        if allowed_kinds.iter().any(|kind| kind.as_str() == "*") {
            Self::Any
        } else {
            Self::Only(allowed_kinds.to_vec())
        }
    }

    pub(super) fn allows(&self, kind: &AgentKind) -> bool {
        match self {
            Self::Any => true,
            Self::Only(kinds) => kinds.iter().any(|allowed| allowed == kind),
        }
    }

    pub(super) fn is_known(kind: &AgentKind) -> bool {
        agent_catalog::find(kind).is_some()
    }

    pub(super) fn catalog(&self) -> Vec<(AgentKind, &'static str)> {
        match self {
            Self::Any => agent_catalog::entries()
                .iter()
                .map(|entry| (entry.kind().clone(), entry.description()))
                .collect(),
            Self::Only(kinds) => kinds
                .iter()
                .filter_map(|kind| {
                    agent_catalog::find(kind).map(|entry| (kind.clone(), entry.description()))
                })
                .collect(),
        }
    }

    pub(super) fn describe(&self) -> String {
        match self {
            Self::Any => "any kind".to_string(),
            Self::Only(kinds) if kinds.is_empty() => "(none)".to_string(),
            Self::Only(kinds) => kinds
                .iter()
                .map(AgentKind::as_str)
                .collect::<Vec<_>>()
                .join(", "),
        }
    }
}
