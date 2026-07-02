//! Capability registry -> agent resolver bridge.
//!
//! Callers describe their device in terms of one concept — the [`Capability`]
//! (a tool, a channel, or a pure lifecycle service) — and register them in a
//! [`Registry`]. This module adapts tool capabilities onto the resolver boundary
//! the agent runtime consumes.
//!
//! - [`RegistryResolver`] — an [`AgentResolver`] whose tools are the registry's
//!   currently-available [`Tool`]s (capability name -> `Tool`). Skills are an
//!   orthogonal concern owned by `claw-skill`; an optional [`SkillRegistry`] is
//!   threaded through unchanged.
//! Wire a registry into an [`AgentSystem`](crate::AgentSystem) by passing it to
//! [`AgentSystem::new`](crate::AgentSystem::new): it installs this resolver.
//! Channel routing is handled by `ChannelRouter`.

use std::sync::Arc;

use claw_capability::Registry;
use claw_core::agent::AgentResolver;
use claw_skill::{SkillError, SkillId, SkillRegistry, SkillSet};
use claw_tool::Tool;

/// Provenance label applied to every skill a manifest asks for.
const MANIFEST_SKILL_GROUP: &str = "manifest";

/// An [`AgentResolver`] backed by the capability [`Registry`].
///
/// Capability names resolve to the registry's available [`Tool`]s; skills resolve
/// through an optional [`SkillRegistry`] (capabilities and skills are
/// independent). Construct with [`new`](Self::new), add skill support with
/// [`with_skill_registry`](Self::with_skill_registry), and share as
/// `Arc<dyn AgentResolver>`.
pub struct RegistryResolver {
    registry: Arc<Registry>,
    skills: Option<Arc<dyn SkillRegistry>>,
}

impl RegistryResolver {
    /// A resolver over `registry` with no skill backing.
    pub fn new(registry: Arc<Registry>) -> Self {
        Self {
            registry,
            skills: None,
        }
    }

    /// Back skills with `registry`: manifest skill ids are loaded from it.
    #[must_use]
    pub fn with_skill_registry(mut self, registry: Arc<dyn SkillRegistry>) -> Self {
        self.skills = Some(registry);
        self
    }
}

impl AgentResolver for RegistryResolver {
    fn resolve_tool(&self, name: &str) -> Option<Tool> {
        self.registry.tool(name)
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
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    use claw_capability::Capability;
    use claw_interface::StdThread;
    use claw_tool::{
        init_tool_executor, AsyncToolHandler, ToolFuture, ToolHandler, ToolInvocation,
        ToolInvokeError, ToolOutput, ToolRunner, ToolSet,
    };

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

    struct AsyncDummyTool;
    impl AsyncToolHandler for AsyncDummyTool {
        fn name(&self) -> &str {
            "do_async"
        }

        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"do_async","parameters":{"type":"object","properties":{}}}}"#
        }

        fn invoke_async<'a>(&'a self, _call: &'a ToolInvocation<'_>) -> ToolFuture<'a> {
            Box::pin(async {
                Ok(ToolOutput {
                    output: "async-ok".into(),
                    ok: true,
                })
            })
        }
    }

    #[test]
    fn resolver_resolves_registered_tool() {
        let registry = Arc::new(Registry::new());
        registry
            .register(Capability::tool(Tool::new(DummyTool)))
            .unwrap();

        let resolver = RegistryResolver::new(Arc::clone(&registry));
        assert!(resolver.resolve_tool("do_thing").is_some());
        assert!(resolver.resolve_tool("missing").is_none());
    }

    #[test]
    fn resolver_runs_registered_async_tool_capability() {
        init_tool_executor(StdThread).expect("tool executor");

        let registry = Arc::new(Registry::new());
        registry
            .register(Capability::async_tool(AsyncDummyTool))
            .unwrap();
        registry.start_all().unwrap();

        let resolver = RegistryResolver::new(Arc::clone(&registry));
        let tool = resolver
            .resolve_tool("do_async")
            .expect("async capability should resolve as a tool");
        let tools = ToolSet::new([tool]).unwrap();
        let runner = ToolRunner::new(&tools, None);

        let outcome = claw_utils::block_on(runner.run_one_async(&ToolInvocation {
            id: Some("t1"),
            name: "do_async",
            arguments_json: "{}",
        }));

        assert!(outcome.ok);
        assert_eq!(outcome.content, "async-ok");
    }

    #[test]
    fn build_skills_without_registry_does_not_silently_drop() {
        let resolver = RegistryResolver::new(Arc::new(Registry::new()));
        assert!(resolver.build_skills(&[]).unwrap().is_none());
        assert!(matches!(
            resolver.build_skills(&[SkillId::new("greet")]),
            Err(SkillError::NotFound(_))
        ));
    }
}
