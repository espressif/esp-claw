use std::sync::Arc;

use claw_context::{Block, BlockKind};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{Compactor, TranscriptStore};

use crate::agent::base_agent::{AgentCommand, BaseAgent, BaseAgentConfig};
use crate::agent::config::{AgentConfig, AgentConfigError};
use crate::agent::tools::discovery_tools;
use crate::config::catalog as agent_catalog;
use crate::memory::{
    CompactionPolicy, ConversationHistoryContextAdapter, LlmCompactor,
    LongTermMemoryContextAdapter, ProfileContextAdapter,
};
use crate::protocol::{AgentId, AgentKind, Message};

use super::error::FsAgentCreateError;
use super::{AgentEnvironment, FsAgentFactory, ProfileAccess};

const COMPACTION_TRIGGER_TOKENS: usize = 6000;
const COMPACTION_KEEP_RECENT_TOKENS: usize = 2000;
const COMPACTION_SEGMENT_TOKEN_BUDGET: usize = 1500;

impl<
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + StreamingHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<Filesystem, Http, Timer>
{
    /// Build one agent of `kind`, already tasked with `goal`.
    ///
    /// Its owner supplies storage, profile access, inherited context, and any
    /// extension tools through `environment`. The factory does not interpret
    /// orchestration roles.
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
        goal: Message,
        environment: AgentEnvironment,
    ) -> Result<BaseAgent<Http, Timer>, FsAgentCreateError> {
        let span = tracing::info_span!("agent.create");
        let _enter = span.enter();
        // The config is pure data. Registry tools are projected here, then
        // filtered by the manifest's tool-group allowlist before the agent sees them.
        let config = self.resolve_config(kind).map_err(|error| {
            match &error {
                AgentConfigError::UnknownKind(_) => {
                    tracing::error!(name: "unknown_kind", kind = %kind.as_str());
                }
            }
            FsAgentCreateError::Config(error)
        })?;
        let mut tools = self.tools.tool_set();
        tools.retain_registry_groups(config.tool_groups);
        tools.add_group(discovery_tools(tools.discovery()))?;
        for extension in environment.extension_tools {
            tools.add_group(extension)?;
        }

        // Every agent gets a transcript for context management; `persists` only
        // decides whether it is written to disk.
        let transcript_id = environment.transcript.id();
        let store = if environment.transcript.persists() {
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
        let conversation_history_store = store.clone();

        // This is the single configured-agent assembly point. BaseAgent adds
        // only its invariant built-ins (skill projection and conversation_end).
        let base_config = BaseAgentConfig {
            store,
            tools,
            skills: config.skills,
            agent_instruction: Block::new(BlockKind::AgentInstruction, config.system_prompt),
            inherited_context: environment.inherited_context,
            retry_policy: config.retry_policy,
            block_retries: config.tool_block_retries,
        };
        let mut agent = match BaseAgent::<Http, Timer>::build(base_config) {
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

        let compactor: Box<dyn Compactor> = Box::new(LlmCompactor::<Http, Timer>::new(Arc::clone(
            &self.api_manager,
        )));
        let conversation_history = ConversationHistoryContextAdapter::new(
            conversation_history_store,
            compactor,
            CompactionPolicy::new(
                COMPACTION_TRIGGER_TOKENS,
                COMPACTION_KEEP_RECENT_TOKENS,
                COMPACTION_SEGMENT_TOKEN_BUDGET,
            ),
        );
        if let Err(error) = agent.register_context_adapter(Box::new(conversation_history)) {
            tracing::error!(
                name: "context_adapter_attach_failed",
                agent = %id,
                adapter = "conversation_history",
                kind = %kind.as_str(),
            );
            return Err(FsAgentCreateError::Agent(error));
        }

        let profile_adapter = ProfileContextAdapter::new(
            self.profile.clone(),
            environment.profile == ProfileAccess::Writable,
        );
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
        let agent_memory = match long_term.agent_store(kind.as_str()) {
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

        if !goal.as_str().trim().is_empty() {
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
        Ok(agent)
    }

    fn resolve_config(&self, kind: &AgentKind) -> Result<AgentConfig, AgentConfigError> {
        let manifest = agent_catalog::find(kind)
            .ok_or_else(|| AgentConfigError::UnknownKind(kind.as_str().to_owned()))?;
        let runtime = manifest.runtime();
        if !runtime.skills().is_empty() {
            tracing::info!(
                name: "manifest_ids_catalog_only",
                count = runtime.skills().len() as u64,
            );
        }
        Ok(AgentConfig::from_manifest(runtime, self.skills.skill_set()))
    }
}
