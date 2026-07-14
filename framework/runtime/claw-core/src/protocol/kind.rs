//! The catalog identity of an agent template.

use std::borrow::Cow;

/// Which agent template to instantiate from `resources/agents/<kind>/`.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(crate) struct AgentKind(Cow<'static, str>);

impl AgentKind {
    pub(crate) fn new(kind: String) -> Self {
        Self(Cow::Owned(kind))
    }

    pub(crate) const fn from_static(kind: &'static str) -> Self {
        Self(Cow::Borrowed(kind))
    }

    pub(crate) fn as_str(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Display for AgentKind {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.0)
    }
}
