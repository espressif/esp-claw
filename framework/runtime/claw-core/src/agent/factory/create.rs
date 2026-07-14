use std::sync::Arc;

use claw_context::{Block, BlockKind};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{Compactor, TranscriptStore};

use crate::agent::base_agent::{AgentCommand, AgentId, BaseAgent, BaseAgentConfig};
use crate::agent::config::{AgentConfig, AgentConfigError};
use crate::agent::graph::{AgentContext, GraphHost};
use crate::agent::kind::AgentKind;
use crate::agent::manifest::AgentManifest;
use crate::agent::tools::{discovery_tools, subagent_tools};
use crate::memory::{
    CompactionPolicy, ConversationHistoryContextAdapter, LlmCompactor,
    LongTermMemoryContextAdapter, ProfileContextAdapter,
};
use crate::session::Message;

use super::error::FsAgentCreateError;
use super::layout::AgentPlacement;
use super::FsAgentFactory;

const COMPACTION_TRIGGER_TOKENS: usize = 6000;
const COMPACTION_KEEP_RECENT_TOKENS: usize = 2000;
const COMPACTION_SEGMENT_TOKEN_BUDGET: usize = 1500;

impl<
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + StreamingHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<Filesystem, Http, Timer>
{
    /// Build an agent of `kind` with id `id` already tasked with `goal`, handing
    /// it `host` as its back-channel to the agent graph. Used for both spawned
    /// subagents and a session's root agent.
    ///
    /// `placement` selects the transcript identity and storage mode. It also
    /// decides root-only tool wiring, so root/subagent identity has one source
    /// of truth.
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
        placement: AgentPlacement,
        host: Arc<dyn GraphHost>,
        inherited_context: Vec<Block<'static>>,
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
        let manifest = AgentManifest::for_kind(kind.as_str()).ok_or_else(|| {
            FsAgentCreateError::Config(AgentConfigError::UnknownKind(kind.as_str().to_owned()))
        })?;
        let is_root = placement.is_root();
        let mut tools = self.tools.tool_set();
        tools.retain_registry_groups(manifest.tool_groups);
        tools.add_group(discovery_tools(tools.discovery()))?;
        let graph_context = Arc::new(AgentContext::new(id, host));
        if config.spawn_enabled {
            tools.add_group(subagent_tools(graph_context, config.spawn_policy))?;
        }

        // Every agent gets a transcript for context management; `persists` only
        // decides whether it is written to disk.
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
        let conversation_history_store = store.clone();

        // This is the single configured-agent assembly point. BaseAgent adds
        // only its invariant built-ins (skill projection and conversation_end).
        let base_config = BaseAgentConfig {
            store,
            tools,
            skills: config.skills,
            agent_instruction: Block::new(BlockKind::AgentInstruction, config.system_prompt),
            inherited_context,
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
        let manifest = AgentManifest::for_kind(kind.as_str())
            .ok_or_else(|| AgentConfigError::UnknownKind(kind.as_str().to_owned()))?;
        if !manifest.skills.is_empty() {
            tracing::info!(
                name: "manifest_ids_catalog_only",
                count = manifest.skills.len() as u64,
            );
        }
        Ok(AgentConfig::from_manifest(
            manifest,
            self.skills.skill_set(),
        ))
    }
}
