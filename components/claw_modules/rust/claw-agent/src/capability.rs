//! Capability -> orchestrator bridge.
//!
//! Callers describe their device in terms of one concept — the [`Capability`]
//! (a tool, a channel, or a pure lifecycle service) — and register them in a
//! [`Registry`]. This module adapts that registry onto the two internal seams
//! the runtime actually consumes:
//!
//! - [`RegistryResolver`] — an [`AgentResolver`] whose tools are the registry's
//!   currently-available [`Tool`]s (capability name -> `Tool`). Skills are an
//!   orthogonal concern owned by `claw-skill`; an optional [`SkillRegistry`] is
//!   threaded through unchanged.
//! - [`RegistryChannelTransport`] — wraps one channel [`ChannelAdapter`] as a
//!   [`ChannelTransport`] for the egress hub, converting the (field-identical)
//!   outbound message types at the boundary so `claw-capability` keeps no upward
//!   dependency on `claw-core`.
//!
//! Wire a registry into an [`AgentSystem`](crate::AgentSystem) with
//! [`AgentSystemBuilder::capabilities`](crate::AgentSystemBuilder::capabilities):
//! it installs the resolver and registers every available channel. Inbound
//! messages flow the other way — push them through
//! [`AgentSystem::ingress`](crate::AgentSystem::ingress).

use std::sync::Arc;

use claw_capability::{ChannelAdapter, OutboundMessage as CapabilityOutbound, Registry};
use claw_core::agent::AgentResolver;
use claw_core::{
    ChannelEgressHub, ChannelError, ChannelTransport, OutboundMessage as CoreOutbound, SkillError,
    SkillId, SkillRegistry, SkillSet, Tool,
};

/// Provenance label applied to every skill a manifest asks for (mirrors
/// `MapAgentResolver`'s group tag).
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

/// A [`ChannelTransport`] backed by a capability [`ChannelAdapter`].
///
/// Adapts the egress hub's outbound type to the adapter's, keeping the two
/// (field-identical) `OutboundMessage` types from coupling their crates.
pub struct RegistryChannelTransport {
    adapter: Arc<dyn ChannelAdapter>,
}

impl RegistryChannelTransport {
    /// Wrap `adapter` as a transport.
    pub fn new(adapter: Arc<dyn ChannelAdapter>) -> Self {
        Self { adapter }
    }
}

impl ChannelTransport for RegistryChannelTransport {
    fn id(&self) -> &str {
        self.adapter.channel_id()
    }

    fn send(&self, message: &CoreOutbound) -> Result<(), ChannelError> {
        let converted = CapabilityOutbound {
            channel: message.channel.clone(),
            chat_id: message.chat_id.clone(),
            text: message.text.clone(),
            reply_to_message_id: message.reply_to_message_id.clone(),
        };
        self.adapter
            .send(&converted)
            .map_err(|error| ChannelError::SendFailed(error.to_string()))
    }
}

/// Register every available channel in `registry` as a [`ChannelTransport`] in
/// `hub`. Call after the registry's channels are started so the egress hub can
/// route replies to them. Returns the number of channels registered.
pub fn register_channels(registry: &Registry, hub: &ChannelEgressHub) -> usize {
    let channels = registry.channels();
    let count = channels.len();
    for adapter in channels {
        hub.register(Arc::new(RegistryChannelTransport::new(adapter)) as Arc<dyn ChannelTransport>);
    }
    count
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    use std::sync::Mutex;

    use claw_capability::{Capability, CapabilityError};
    use claw_core::{ToolInvocation, ToolInvokeError, ToolOutput};

    struct DummyTool;
    impl claw_core::ToolHandler for DummyTool {
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
    fn build_skills_without_registry_does_not_silently_drop() {
        let resolver = RegistryResolver::new(Arc::new(Registry::new()));
        assert!(resolver.build_skills(&[]).unwrap().is_none());
        assert!(matches!(
            resolver.build_skills(&[SkillId::new("greet")]),
            Err(SkillError::NotFound(_))
        ));
    }

    struct RecordingAdapter {
        id: String,
        sent: Mutex<Vec<CapabilityOutbound>>,
    }
    impl ChannelAdapter for RecordingAdapter {
        fn channel_id(&self) -> &str {
            &self.id
        }
        fn send(&self, message: &CapabilityOutbound) -> Result<(), CapabilityError> {
            self.sent.lock().unwrap().push(message.clone());
            Ok(())
        }
    }

    #[test]
    fn transport_converts_and_forwards() {
        let adapter = Arc::new(RecordingAdapter {
            id: "local".to_string(),
            sent: Mutex::new(Vec::new()),
        });
        let transport =
            RegistryChannelTransport::new(Arc::clone(&adapter) as Arc<dyn ChannelAdapter>);
        assert_eq!(transport.id(), "local");

        transport
            .send(&CoreOutbound {
                channel: "local".to_string(),
                chat_id: "chat".to_string(),
                text: "hi".to_string(),
                reply_to_message_id: Some("m1".to_string()),
            })
            .unwrap();

        let sent = adapter.sent.lock().unwrap();
        assert_eq!(sent.len(), 1);
        assert_eq!(sent[0].text, "hi");
        assert_eq!(sent[0].reply_to_message_id.as_deref(), Some("m1"));
    }

    #[test]
    fn register_channels_registers_each_available_channel() {
        let registry = Arc::new(Registry::new());
        registry
            .register(Capability::channel(Arc::new(RecordingAdapter {
                id: "local".to_string(),
                sent: Mutex::new(Vec::new()),
            })))
            .unwrap();

        let hub = ChannelEgressHub::new();
        assert_eq!(register_channels(&registry, &hub), 1);
    }
}
