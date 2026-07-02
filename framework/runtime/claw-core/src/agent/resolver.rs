//! Resolving capability/skill *names* into handler *code*: the [`AgentResolver`]
//! seam and its concrete, data-driven implementation [`MapAgentResolver`].
//!
//! A baked manifest only carries *names*; the handlers live in firmware (or a
//! test double). [`AgentResolver`] is the injected boundary that turns each name
//! into a real [`Tool`] / [`SkillSet`], and [`MapAgentResolver`] is the resolver
//! firmware (and tests) hand to an [`FsAgentFactory`](crate::agent::FsAgentFactory):
//! a capability-name -> [`Tool`] map plus an optional [`SkillRegistry`] backing.
//! An unknown name is never silently dropped — `resolve_tool` returns `None` (so
//! the config build fails with `UnknownCapability`) and `build_skills` returns
//! [`SkillError::NotFound`].

use std::collections::HashMap;
use std::sync::Arc;

use claw_skill::{SkillError, SkillId, SkillRegistry, SkillSet};
use claw_tool::Tool;

/// Resolves the *names* in a manifest to the *code* that backs them.
///
/// A manifest only carries capability/skill names; the handlers live in firmware
/// (or a test double). The resolver is the injected boundary that turns each name
/// into a real [`Tool`] / [`SkillSet`]. An unknown name is **not** silently
/// dropped — the resolver returns `None`/an error and resolution fails.
pub trait AgentResolver {
    /// Resolve a capability name to its [`Tool`], or `None` if this resolver has
    /// no such capability.
    fn resolve_tool(&self, name: &str) -> Option<Tool>;

    /// Build a [`SkillSet`] with `skill_ids` loaded.
    ///
    /// Returns `Ok(None)` when no skills are configured (`skill_ids` empty) or the
    /// resolver has no skill support; `Ok(Some(set))` once the ids are loaded.
    ///
    /// # Errors
    ///
    /// [`SkillError`] if a requested skill id is unknown to the resolver.
    fn build_skills(&self, skill_ids: &[SkillId]) -> Result<Option<SkillSet>, SkillError>;
}

/// Group label applied to every skill a manifest asks for (parallels a
/// `ToolGroup` name — it tags provenance in the assembled skill context).
const MANIFEST_SKILL_GROUP: &str = "manifest";

/// Maps capability names to [`Tool`]s and skill ids to a [`SkillRegistry`].
///
/// Built up with the `with_*` methods (easy default + readable overrides), then
/// shared as `Arc<dyn AgentResolver>`:
///
/// ```ignore
/// let resolver = MapAgentResolver::new()
///     .with_tool(Tool::new(MyCapability))
///     .with_skill_registry(registry);
/// ```
#[derive(Default)]
pub struct MapAgentResolver {
    tools: HashMap<String, Tool>,
    skills: Option<Arc<dyn SkillRegistry>>,
}

impl MapAgentResolver {
    /// An empty resolver: no capabilities, no skill backing.
    pub fn new() -> Self {
        Self::default()
    }

    /// Register `tool` under its own [`name`](Tool::name) — the name a manifest
    /// references it by. Replaces any tool already registered under that name.
    #[must_use]
    pub fn with_tool(mut self, tool: Tool) -> Self {
        self.tools.insert(tool.name().to_string(), tool);
        self
    }

    /// Register `tool` under an explicit `name` (when the manifest name differs
    /// from the tool's own function name).
    #[must_use]
    pub fn with_named_tool(mut self, name: impl Into<String>, tool: Tool) -> Self {
        self.tools.insert(name.into(), tool);
        self
    }

    /// Register every tool in `tools`, each keyed by its own [`name`](Tool::name).
    #[must_use]
    pub fn with_tools(mut self, tools: impl IntoIterator<Item = Tool>) -> Self {
        for tool in tools {
            self.tools.insert(tool.name().to_string(), tool);
        }
        self
    }

    /// Back skills with `registry`: manifest skill ids are loaded from it.
    #[must_use]
    pub fn with_skill_registry(mut self, registry: Arc<dyn SkillRegistry>) -> Self {
        self.skills = Some(registry);
        self
    }
}

impl AgentResolver for MapAgentResolver {
    fn resolve_tool(&self, name: &str) -> Option<Tool> {
        self.tools.get(name).cloned()
    }

    fn build_skills(&self, skill_ids: &[SkillId]) -> Result<Option<SkillSet>, SkillError> {
        // No ids requested: a genuine "no skills", not an error.
        let Some(first) = skill_ids.first() else {
            return Ok(None);
        };
        // Ids requested but no registry to load them from is a misconfiguration,
        // not "no skills" — surface the first offender rather than dropping it.
        let Some(registry) = self.skills.clone() else {
            return Err(SkillError::NotFound(first.clone()));
        };
        let mut set = SkillSet::new(registry);
        for id in skill_ids {
            set.load(MANIFEST_SKILL_GROUP, id.clone())?;
        }
        Ok(Some(set))
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use super::*;
    use claw_tool::{ToolHandler, ToolInvocation, ToolOutput};

    struct DummyTool;

    impl ToolHandler for DummyTool {
        fn name(&self) -> &str {
            "do_thing"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"do_thing"}}"#
        }
        fn invoke(
            &self,
            _call: &ToolInvocation<'_>,
        ) -> Result<ToolOutput, claw_tool::ToolInvokeError> {
            Ok(ToolOutput {
                output: "ok".into(),
                ok: true,
            })
        }
    }

    #[test]
    fn known_capability_resolves_unknown_is_none() {
        let resolver = MapAgentResolver::new().with_tool(Tool::new(DummyTool));
        assert!(resolver.resolve_tool("do_thing").is_some());
        assert!(resolver.resolve_tool("missing").is_none());
    }

    #[test]
    fn named_alias_overrides_the_key() {
        let resolver = MapAgentResolver::new().with_named_tool("aliased", Tool::new(DummyTool));
        assert!(resolver.resolve_tool("aliased").is_some());
        // The tool's own name is not registered when an alias is used.
        assert!(resolver.resolve_tool("do_thing").is_none());
    }

    #[test]
    fn no_skills_requested_is_ok_none() {
        let resolver = MapAgentResolver::new();
        assert!(resolver.build_skills(&[]).unwrap().is_none());
    }

    #[test]
    fn skills_requested_without_a_registry_is_an_error() {
        let resolver = MapAgentResolver::new();
        let result = resolver.build_skills(&[SkillId::new("greet")]);
        assert!(matches!(result, Err(SkillError::NotFound(_))));
    }
}
