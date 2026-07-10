use crate::agent::kind::AgentKind;
use crate::agent::manifest::{AgentManifest, MANIFESTS};

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum SpawnPolicy {
    Any,
    Only(Vec<AgentKind>),
}

impl SpawnPolicy {
    pub(crate) fn from_allowed_kinds(allowed_kinds: &[AgentKind]) -> Self {
        if allowed_kinds.iter().any(|kind| kind.as_str() == "*") {
            SpawnPolicy::Any
        } else {
            SpawnPolicy::Only(allowed_kinds.to_vec())
        }
    }

    pub(crate) fn allows(&self, kind: &AgentKind) -> bool {
        match self {
            SpawnPolicy::Any => true,
            SpawnPolicy::Only(kinds) => kinds.iter().any(|allowed| allowed == kind),
        }
    }

    pub(crate) fn catalog(&self) -> Vec<(AgentKind, &'static str)> {
        match self {
            SpawnPolicy::Any => MANIFESTS
                .iter()
                .map(|manifest| (manifest.kind.clone(), manifest.description))
                .collect(),
            SpawnPolicy::Only(kinds) => kinds
                .iter()
                .filter_map(|kind| {
                    AgentManifest::for_kind(kind.as_str())
                        .map(|manifest| (kind.clone(), manifest.description))
                })
                .collect(),
        }
    }

    pub(crate) fn describe(&self) -> String {
        match self {
            SpawnPolicy::Any => "any kind".to_string(),
            SpawnPolicy::Only(kinds) if kinds.is_empty() => "(none)".to_string(),
            SpawnPolicy::Only(kinds) => kinds
                .iter()
                .map(AgentKind::as_str)
                .collect::<Vec<_>>()
                .join(", "),
        }
    }
}
