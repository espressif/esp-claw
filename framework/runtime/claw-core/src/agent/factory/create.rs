use std::sync::Arc;

use claw_context::Block;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::TranscriptStore;

use crate::agent::base_agent::{AgentCommand, AgentId};
use crate::agent::config::{AgentConfig, AgentConfigError};
use crate::agent::generic_agent::GenericAgent;
use crate::agent::graph::GraphHost;
use crate::agent::kind::AgentKind;
use crate::agent::manifest::AgentManifest;
use crate::agent::Agent;
use crate::memory::{agent_store, LongTermMemoryContextAdapter, ProfileContextAdapter};

use super::error::FsAgentCreateError;
use super::layout::{join_storage_path, AgentPlacement};
use super::FsAgentFactory;

impl<
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<Filesystem, Http, Timer>
{
    /// Build an agent of `kind` with id `id` already tasked with `goal`, handing
    /// it `host` as its back-channel to the agent graph. Used for both spawned
    /// subagents and a session's root agent.
    ///
    /// `placement` selects the durable transcript this agent attaches to: a
    /// root keys its record by the stable session id (so it resumes across
    /// restarts), a subagent by its agent id. It also decides root-only tool
    /// wiring, so root/subagent identity has one source of truth.
    ///
    /// # Errors
    ///
    /// Returns a typed error when `kind` is unknown or the agent cannot be
    /// assembled; callers decide where to render it for logs or user-facing
    /// errors.
    pub(crate) fn create_agent(
        &self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        placement: AgentPlacement,
        host: Arc<dyn GraphHost>,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Box<dyn Agent>, FsAgentCreateError> {
        let span = tracing::info_span!("agent.create");
        let _enter = span.enter();
        // The config is pure data. Registry tools are projected here, then
        // manifest tools are added as local tools before the agent sees them.
        let mut config = self.resolve_config(kind).map_err(|error| {
            match &error {
                AgentConfigError::UnknownKind(_) => {
                    tracing::error!(name: "unknown_kind", kind = %kind.as_str());
                }
                AgentConfigError::UnknownTool(tool) => {
                    tracing::error!(
                        name: "unknown_tool",
                        kind = %kind.as_str(),
                        tool = %tool,
                    );
                }
            }
            FsAgentCreateError::Config(error)
        })?;
        let is_root = placement.is_root();
        let mut tools = self.tools.tool_set();
        for tool in config.tools.drain(..) {
            if let Err(error) = tools.add_tool(tool) {
                tracing::error!(
                    name: "unknown_tool",
                    kind = %kind.as_str(),
                    tool = "registry",
                );
                return Err(FsAgentCreateError::Tools(error));
            }
        }

        // Every agent gets a transcript for context management; `persists` only
        // decides whether it is written to disk. Roots persist under
        // `transcript/<session id>.jsonl`; subagents stay in memory.
        let transcript_id = placement.transcript_id();
        let store = if placement.persists() {
            match TranscriptStore::<Filesystem>::new(transcript_id, &self.transcript_dir) {
                Ok(store) => store,
                Err(error) => {
                    tracing::error!(
                        name: "transcript_open_failed",
                        agent = %id,
                        kind = %kind.as_str(),
                    );
                    return Err(FsAgentCreateError::Transcript(error));
                }
            }
        } else {
            TranscriptStore::<Filesystem>::in_memory(transcript_id)
        };
        // The LLM client (and its transport) is built inside the agent from this
        // shared config plus the factory's transport type; nothing is minted here.
        let mut agent = match GenericAgent::<Http, Timer>::new(
            id,
            self.llm_config.clone(),
            store,
            config,
            tools,
            host,
            is_root,
            inherited_context,
        ) {
            Ok(agent) => agent,
            Err(error) => {
                tracing::error!(
                    name: "agent_build_failed",
                    agent = %id,
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::Agent(error));
            }
        };

        let profile_adapter = ProfileContextAdapter::new(self.profile.clone(), is_root);
        if let Err(error) = agent.register_context_adapter(Box::new(profile_adapter)) {
            tracing::error!(
                name: "context_adapter_attach_failed",
                agent = %id,
                adapter = "profile",
                kind = %kind.as_str(),
            );
            return Err(FsAgentCreateError::ProfileContext(error));
        }

        let long_term = &self.long_term;
        let agent_dir = join_storage_path(&long_term.agent_root_dir, kind.as_str());
        let agent_memory = match agent_store::<Filesystem>(&agent_dir) {
            Ok(store) => store,
            Err(error) => {
                tracing::error!(
                    name: "context_adapter_attach_failed",
                    agent = %id,
                    adapter = "long_term",
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::LongTerm(error));
            }
        };
        let adapter = LongTermMemoryContextAdapter::new(
            agent_memory,
            long_term.global.clone(),
            Arc::clone(&long_term.classifier),
            Arc::clone(&long_term.extractor),
        );
        if let Err(error) = agent.register_context_adapter(Box::new(adapter)) {
            tracing::error!(
                name: "context_adapter_attach_failed",
                agent = %id,
                adapter = "long_term",
                kind = %kind.as_str(),
            );
            return Err(FsAgentCreateError::LongTermContext(error));
        }

        if !goal.trim().is_empty() {
            if let Err(error) = agent.send_command(AgentCommand::AppendMessage(goal)) {
                tracing::error!(
                    name: "goal_seed_failed",
                    agent = %id,
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::Goal(error));
            }
        }

        tracing::info!(name: "created", agent = %id, kind = %kind.as_str());
        Ok(Box::new(agent))
    }

    fn resolve_config(&self, kind: &AgentKind) -> Result<AgentConfig, AgentConfigError> {
        let manifest = AgentManifest::for_kind(kind.as_str())
            .ok_or_else(|| AgentConfigError::UnknownKind(kind.as_str().to_owned()))?;
        if let Some(name) = manifest.tools.first() {
            return Err(AgentConfigError::UnknownTool(name.as_str().to_owned()));
        }
        if !manifest.skills.is_empty() {
            tracing::info!(
                name: "manifest_ids_catalog_only",
                count = manifest.skills.len() as u64,
            );
        }
        Ok(AgentConfig::from_manifest(
            manifest,
            Vec::new(),
            self.skills.skill_set(),
        ))
    }
}
