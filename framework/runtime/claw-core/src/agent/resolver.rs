//! Resolving capability/skill *names* into handler *code*: the [`AgentResolver`]
//! seam and its concrete, data-driven implementation [`MapAgentResolver`].
//!
//! A baked manifest only carries *names*; the handlers live in firmware (or a
//! test double). [`AgentResolver`] is the injected boundary that turns each name
//! into a real [`Capability`] / [`SkillSet`], and [`MapAgentResolver`] is the
//! resolver firmware (and tests) hand to an
//! [`FsAgentFactory`](crate::agent::FsAgentFactory): a capability-name ->
//! [`Capability`] map plus a [`SkillRegistry`] backing. An unknown name
//! is never silently dropped — `resolve_capability` returns `None` (so the config
//! build fails with `UnknownCapability`) and `build_skills` returns
//! [`SkillError::NotFound`].
//!
//! The seam speaks [`Capability`] rather than the internal `claw_tool::Tool`:
//! callers describe their device in one vocabulary, and `claw-core` decomposes a
//! resolved capability into its tool internally (see `AgentConfig::resolve`).

use std::collections::HashMap;
use std::sync::Arc;

use claw_capability::Capability;
use claw_skill::{EmptySkillRegistry, SkillError, SkillId, SkillRegistry, SkillSet};

/// Resolves the *names* in a manifest to the *code* that backs them.
///
/// A manifest only carries capability/skill names; the handlers live in firmware
/// (or a test double). The resolver is the injected boundary that turns each name
/// into a real [`Capability`] / [`SkillSet`]. An unknown name is **not** silently
/// dropped — the resolver returns `None`/an error and resolution fails.
pub trait AgentResolver {
    /// Resolve a capability name to its [`Capability`], or `None` if this
    /// resolver has no such (tool) capability. `claw-core` extracts the tool from
    /// the returned capability; a non-tool capability resolves as if absent.
    fn resolve_capability(&self, name: &str) -> Option<Capability>;

    /// The [`SkillRegistry`] this resolver loads manifest skills from.
    ///
    /// Override to add skill support; the default is an empty registry, so no
    /// skills configured is represented by an empty [`SkillSet`], while any
    /// requested id still fails with [`SkillError::NotFound`].
    fn skill_registry(&self) -> Arc<dyn SkillRegistry> {
        Arc::new(EmptySkillRegistry)
    }

    /// Build a [`SkillSet`] with `skill_ids` loaded from
    /// [`skill_registry`](Self::skill_registry).
    ///
    /// Returns an empty set when no skills are configured (`skill_ids` empty), or a
    /// set with the requested ids loaded.
    /// The default implementation is shared by every resolver; override only for
    /// genuinely different behavior.
    ///
    /// # Errors
    ///
    /// [`SkillError`] if a requested skill id is unknown to the resolver.
    fn build_skills(&self, skill_ids: &[SkillId]) -> Result<SkillSet, SkillError> {
        build_manifest_skills(self.skill_registry(), skill_ids)
    }
}

/// Group label applied to every skill a manifest asks for (parallels a
/// `ToolGroup` name — it tags provenance in the assembled skill context).
const MANIFEST_SKILL_GROUP: &str = "manifest";

/// Load `skill_ids` from `registry` into a fresh [`SkillSet`], the shared body
/// behind [`AgentResolver::build_skills`].
///
/// Returns an empty set when no ids are requested. A resolver without skill
/// backing uses an empty registry, so requested ids still surface as
/// [`SkillError::NotFound`] rather than being silently dropped.
fn build_manifest_skills(
    registry: Arc<dyn SkillRegistry>,
    skill_ids: &[SkillId],
) -> Result<SkillSet, SkillError> {
    let mut set = SkillSet::new(registry);
    for id in skill_ids {
        set.load(MANIFEST_SKILL_GROUP, id.clone())?;
    }
    Ok(set)
}

/// Maps capability names to [`Capability`]s and skill ids to a [`SkillRegistry`].
///
/// Built up with the `with_*` methods (easy default + readable overrides), then
/// shared as `Arc<dyn AgentResolver>`:
///
/// ```ignore
/// let resolver = MapAgentResolver::new()
///     .with_capability(Capability::from_tool(Tool::new(MyToolHandler)))
///     .with_skill_registry(registry);
/// ```
pub struct MapAgentResolver {
    capabilities: HashMap<String, Capability>,
    skills: Arc<dyn SkillRegistry>,
}

impl Default for MapAgentResolver {
    fn default() -> Self {
        Self {
            capabilities: HashMap::new(),
            skills: Arc::new(EmptySkillRegistry),
        }
    }
}

impl MapAgentResolver {
    /// An empty resolver: no capabilities, no skill backing.
    pub fn new() -> Self {
        Self::default()
    }

    /// Register `capability` under its own [`id`](Capability::id) — the name a
    /// manifest references it by. Replaces any capability already under that name.
    #[must_use]
    pub fn with_capability(mut self, capability: Capability) -> Self {
        self.capabilities
            .insert(capability.id().to_string(), capability);
        self
    }

    /// Register `capability` under an explicit `name` (when the manifest name
    /// differs from the capability's own id).
    #[must_use]
    pub fn with_named_capability(
        mut self,
        name: impl Into<String>,
        capability: Capability,
    ) -> Self {
        self.capabilities.insert(name.into(), capability);
        self
    }

    /// Register every capability in `capabilities`, each keyed by its own
    /// [`id`](Capability::id).
    #[must_use]
    pub fn with_capabilities(mut self, capabilities: impl IntoIterator<Item = Capability>) -> Self {
        for capability in capabilities {
            self.capabilities
                .insert(capability.id().to_string(), capability);
        }
        self
    }

    /// Back skills with `registry`: manifest skill ids are loaded from it.
    #[must_use]
    pub fn with_skill_registry(mut self, registry: Arc<dyn SkillRegistry>) -> Self {
        self.skills = registry;
        self
    }
}

impl AgentResolver for MapAgentResolver {
    fn resolve_capability(&self, name: &str) -> Option<Capability> {
        self.capabilities.get(name).cloned()
    }

    fn skill_registry(&self) -> Arc<dyn SkillRegistry> {
        Arc::clone(&self.skills)
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use super::*;
    use claw_capability::{Tool, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};

    struct DummyTool;

    impl ToolHandler for DummyTool {
        fn name(&self) -> &str {
            "do_thing"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"do_thing"}}"#
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: "ok".into(),
                ok: true,
            })
        }
    }

    #[test]
    fn known_capability_resolves_unknown_is_none() {
        let resolver =
            MapAgentResolver::new().with_capability(Capability::from_tool(Tool::new(DummyTool)));
        assert!(resolver.resolve_capability("do_thing").is_some());
        assert!(resolver.resolve_capability("missing").is_none());
    }

    #[test]
    fn named_alias_overrides_the_key() {
        let resolver = MapAgentResolver::new()
            .with_named_capability("aliased", Capability::from_tool(Tool::new(DummyTool)));
        assert!(resolver.resolve_capability("aliased").is_some());
        // The capability's own id is not registered when an alias is used.
        assert!(resolver.resolve_capability("do_thing").is_none());
    }

    #[test]
    fn no_skills_requested_is_an_empty_set() {
        let resolver = MapAgentResolver::new();
        assert!(resolver.build_skills(&[]).unwrap().is_empty());
    }

    #[test]
    fn skills_requested_without_a_registry_is_an_error() {
        let resolver = MapAgentResolver::new();
        let result = resolver.build_skills(&[SkillId::new("greet")]);
        assert!(matches!(result, Err(SkillError::NotFound(_))));
    }
}
