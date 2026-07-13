//! [`AgentKind`] — the *type* identity of an agent (its role/template), as
//! opposed to [`AgentId`](crate::agent::base_agent::AgentId), the *instance*
//! identity.

use std::borrow::Cow;

/// Which agent *template* (role) to instantiate — the directory name under
/// `resources/agents/<kind>/`.
///
/// A kind is a **type**, one-to-many with running instances: every spawned agent
/// gets a unique [`AgentId`](crate::agent::base_agent::AgentId), but many of them
/// can share the same `AgentKind`. The spawning model picks the kind when it
/// delegates.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub(crate) struct AgentKind(Cow<'static, str>);

impl AgentKind {
    /// Wrap a runtime role name as a kind (owns its `String`).
    pub(crate) fn new(kind: String) -> Self {
        Self(Cow::Owned(kind))
    }

    /// Wrap a `&'static str` as a kind in a `const` context (no allocation) —
    /// used by build-script-generated manifests.
    pub(crate) const fn from_static(kind: &'static str) -> Self {
        Self(Cow::Borrowed(kind))
    }

    /// The kind as a string slice.
    pub(crate) fn as_str(&self) -> &str {
        &self.0
    }
}

impl std::fmt::Display for AgentKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}
