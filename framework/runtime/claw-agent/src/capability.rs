//! Capability registry -> agent resolver bridge.
//!
//! Callers describe their device in terms of one concept — the [`Capability`]
//! (a tool, a channel, or a pure lifecycle service) — and register them in a
//! [`Registry`]. This module adapts tool capabilities onto the resolver boundary
//! the agent runtime consumes.
//!
//! - [`RegistryResolver`] — an [`AgentResolver`] whose capabilities are the
//!   registry's currently-available tool [`Capability`]s (capability name ->
//!   `Capability`). Skills are an orthogonal concern owned by `claw-skill`; a
//!   resolver without skill support carries an empty [`SkillRegistry`].
//!
//! Wire a registry into an [`AgentSystem`](crate::AgentSystem) by passing it to
//! [`AgentSystem::new`](crate::AgentSystem::new): it installs this resolver.
//! Channel routing is handled by `ChannelRouter`.

use std::sync::Arc;

use claw_capability::{Capability, Registry};
use claw_core::AgentResolver;
use claw_skill::{EmptySkillRegistry, SkillRegistry};

/// An [`AgentResolver`] backed by the capability [`Registry`].
///
/// Capability names resolve to the registry's available tool [`Capability`]s;
/// skills resolve through a [`SkillRegistry`] (capabilities and skills are
/// independent). Construct with [`new`](Self::new), add skill support with
/// [`with_skill_registry`](Self::with_skill_registry), and share as
/// `Arc<dyn AgentResolver>`.
pub struct RegistryResolver {
    registry: Arc<Registry>,
    skills: Arc<dyn SkillRegistry>,
}

impl RegistryResolver {
    /// A resolver over `registry` with no skill backing.
    pub fn new(registry: Arc<Registry>) -> Self {
        Self {
            registry,
            skills: Arc::new(EmptySkillRegistry),
        }
    }

    /// Back skills with `registry`: manifest skill ids are loaded from it.
    #[must_use]
    pub fn with_skill_registry(mut self, registry: Arc<dyn SkillRegistry>) -> Self {
        self.skills = registry;
        self
    }
}

impl AgentResolver for RegistryResolver {
    fn resolve_capability(&self, name: &str) -> Option<Capability> {
        self.registry.tool_capability(name)
    }

    fn skill_registry(&self) -> Arc<dyn SkillRegistry> {
        Arc::clone(&self.skills)
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    use claw_capability::{CapabilityRole, ToolInvocation};
    use claw_interface::StdThread;
    use claw_skill::{SkillError, SkillId};
    use claw_tool::{
        init_tool_executor, AsyncToolHandler, Tool, ToolFuture, ToolHandler, ToolInvokeError,
        ToolOutput, ToolRunner, ToolSet,
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
            .register(Capability::from_tool(Tool::new(DummyTool)))
            .unwrap();

        let resolver = RegistryResolver::new(Arc::clone(&registry));
        assert!(resolver.resolve_capability("do_thing").is_some());
        assert!(resolver.resolve_capability("missing").is_none());
    }

    #[test]
    fn resolver_runs_registered_async_tool_capability() {
        init_tool_executor(StdThread).expect("tool executor");

        let registry = Arc::new(Registry::new());
        registry
            .register(Capability::from_tool(Tool::new_async(AsyncDummyTool)))
            .unwrap();
        registry.start_all().unwrap();

        let resolver = RegistryResolver::new(Arc::clone(&registry));
        let capability = resolver
            .resolve_capability("do_async")
            .expect("async capability should resolve as a tool");
        let CapabilityRole::Tool(tool) = capability.role().clone() else {
            panic!("resolved capability should carry a tool role");
        };
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
        assert!(resolver.build_skills(&[]).unwrap().is_empty());
        assert!(matches!(
            resolver.build_skills(&[SkillId::new("greet")]),
            Err(SkillError::NotFound(_))
        ));
    }
}
