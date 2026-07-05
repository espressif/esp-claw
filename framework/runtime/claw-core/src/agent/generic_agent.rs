//! The single generic, flat agent — one runtime, configured by data.
//!
//! There is no semantic FSM. [`GenericAgent`] is a thin wrapper over a
//! [`BaseAgent`](crate::agent::base_agent::BaseAgent): it forwards commands and
//! ticks straight through, and the only thing that distinguishes one agent
//! "kind" from another is its [`AgentConfig`] — the system prompt, the tool set,
//! the skills, and whether it may spawn. The model drives its own flow (ReAct):
//! it reads, acts, and answers freely, ending a task only via the built-in
//! `end_conversation` tool.
//!
//! Where an [`AgentConfig`] comes from lives in the factory. This module only
//! consumes the resolved config.

use std::sync::Arc;

use claw_api::{ClawApiAsync, ClawApiConfig, InitError};
use claw_context::{Block, BlockKind};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{Compactor, TranscriptStore};
use claw_permission::AllowAll;
use claw_tool::{ToolSet, ToolSetError};

use crate::agent::base_agent::{
    AgentAbortHandle, AgentCommand, AgentCommandError, AgentId, BaseAgent, BaseAgentBuildError,
    BaseAgentConfig,
};
use crate::agent::config::AgentConfig;
use crate::agent::graph::{AgentContext, GraphHost};
use crate::agent::tools::subagent_tools;
use crate::agent::{Agent, AgentTickFuture};
use crate::memory::{
    CompactionPolicy, ContextAdapter, LlmCompactor, RecentMessagesContextAdapter,
    RollingSummaryContextAdapter, SummaryCursor,
};

const COMPACTION_TRIGGER_TOKENS: usize = 6000;
const COMPACTION_KEEP_RECENT_TOKENS: usize = 2000;
const COMPACTION_SEGMENT_TOKEN_BUDGET: usize = 1500;

// ===========================================================================
// GenericAgent: the flat Agent over BaseAgent
// ===========================================================================

/// The one agent type: a flat ReAct loop over a [`BaseAgent`], configured by an
/// [`AgentConfig`]. No semantic FSM — `tick` forwards straight to the base.
pub struct GenericAgent<H: ClawHttp, Timer: ClawTimer> {
    id: AgentId,
    base: BaseAgent<H, Timer>,
}

impl<H: ClawHttp, Timer: ClawTimer> GenericAgent<H, Timer> {
    /// Build a generic agent with `id`, configured by `config`.
    ///
    /// The LLM client is built inside the base agent from `llm_config` over the
    /// injected `http`/`timer` transports — the caller passes configuration, not a
    /// pre-constructed client.
    ///
    /// The transcript store is supplied by the caller. The factory owns durable
    /// storage layout decisions (root session id vs. subagent id, directory
    /// selection, and filesystem handle); this layer only wires that store into
    /// the agent's context strategy.
    ///
    /// Conversation compaction belongs to this agent layer, not the store: the
    /// store remains verbatim storage, while this constructor wires the recent
    /// history adapter and rolling-summary adapter as one internal strategy.
    ///
    /// `tools` is prepared by the factory from the central tool registry plus
    /// manifest-local tools. This layer only adds graph tools that require a
    /// [`GraphHost`]: `spawn_subagent` and its inspection/delete siblings when
    /// `config.spawn_enabled`. The base agent then adds its built-in self-control
    /// tool (`end_conversation`).
    ///
    /// `inherited_context` is the scope-layered prose injected from above
    /// (Global -> Session), handed straight to the base agent so it renders ahead
    /// of the agent's own blocks. Empty for a standalone agent.
    ///
    /// # Errors
    ///
    /// [`GenericAgentBuildError`] when tool assembly or base construction fails.
    pub fn new<F: ClawFs + 'static>(
        id: AgentId,
        llm_config: ClawApiConfig,
        store: TranscriptStore<F>,
        config: AgentConfig,
        mut tools: ToolSet,
        host: Arc<dyn GraphHost>,
        _is_root: bool,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Self, GenericAgentBuildError>
    where
        H: Default + 'static,
        Timer: Default + 'static,
    {
        // The two conversation adapters share one cursor: the rolling summary
        // advances it as it summarizes; the recent-tail adapter renders only the
        // turns past it. This conversation-memory policy belongs to GenericAgent,
        // not BaseAgent.
        let compaction_llm_config = llm_config.clone();
        let cursor = SummaryCursor::new();
        let recent = RecentMessagesContextAdapter::new(store.clone(), cursor.clone());
        let rolling_summary_store = store.clone();

        // Graph-affecting tools need a back-channel.
        let context = Arc::new(AgentContext::new(id, host));
        if config.spawn_enabled {
            for tool in subagent_tools(Arc::clone(&context), config.spawn_policy) {
                tools.add_tool(tool)?;
            }
        }

        // The soft-hide "retry then fail" budget is the agent's BlockPolicy.
        let base_config = BaseAgentConfig {
            llm_config,
            store,
            tools,
            skills: config.skills,
            agent_instruction: Block::new(BlockKind::AgentInstruction, config.system_prompt),
            inherited_context,
            retry_policy: config.retry_policy,
            permission_policy: Arc::new(AllowAll),
            block_retries: config.tool_block_retries,
        };
        let mut base = BaseAgent::build(base_config)?;
        let compaction_llm =
            ClawApiAsync::init(compaction_llm_config, H::default(), Timer::default())
                .map_err(GenericAgentBuildError::CompactionLlm)?;
        let compactor: Arc<dyn Compactor> = Arc::new(LlmCompactor::new(compaction_llm));
        let rolling_summary = RollingSummaryContextAdapter::new(
            rolling_summary_store,
            compactor,
            CompactionPolicy::new(
                COMPACTION_TRIGGER_TOKENS,
                COMPACTION_KEEP_RECENT_TOKENS,
                COMPACTION_SEGMENT_TOKEN_BUDGET,
            ),
            cursor,
        );
        // Attach both halves of the conversation-history projection here so the
        // low-level base agent remains free of a specific memory strategy.
        base.register_context_adapter(Box::new(recent))?;
        base.register_context_adapter(Box::new(rolling_summary))?;

        Ok(Self { id, base })
    }

    /// Register a pluggable [`ContextAdapter`] on the underlying base agent.
    ///
    /// Forwards to [`BaseAgent::register_context_adapter`]; the factory calls this
    /// after construction to attach the dual-tier long-term store.
    ///
    /// # Errors
    ///
    /// [`BaseAgentBuildError`] when an adapter tool clashes with an existing tool
    /// or the LLM does not support the tools the adapter provides.
    pub fn register_context_adapter(
        &mut self,
        adapter: Box<dyn ContextAdapter>,
    ) -> Result<(), BaseAgentBuildError> {
        self.base.register_context_adapter(adapter)
    }
}

impl<H: ClawHttp, Timer: ClawTimer> Agent for GenericAgent<H, Timer> {
    fn id(&self) -> AgentId {
        self.id
    }

    fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        self.base.send_command(command)
    }

    fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool) {
        append_child_result(&mut self.base, child, text, ok);
    }

    fn deliver_child_input(&mut self, child: AgentId, text: String) {
        append_child_input(&mut self.base, child, text);
    }

    fn abort_handle(&self) -> AgentAbortHandle {
        self.base.abort_handle()
    }

    fn tick(&mut self) -> AgentTickFuture<'_> {
        // Flat: no FSM, no per-phase gating — the model drives its own flow.
        Box::pin(async move { self.base.tick().await })
    }
}

/// Failure assembling a [`GenericAgent`].
#[derive(Debug, thiserror::Error)]
pub enum GenericAgentBuildError {
    /// Assembling the tool set failed (e.g. a duplicate tool name).
    #[error(transparent)]
    Tools(#[from] ToolSetError),
    /// The underlying [`BaseAgent`] could not be built.
    #[error(transparent)]
    Base(#[from] BaseAgentBuildError),
    /// The compaction LLM client could not be initialized from the supplied
    /// [`ClawApiConfig`].
    #[error("failed to initialize the compaction LLM client: {0}")]
    CompactionLlm(#[source] InitError),
}

/// Shared: present a subagent's result as a provenance-tagged message and append
/// it to the agent's base memory.
///
/// Child results re-enter the conversation as information the model re-decides
/// over (no counting, no gating); both semantic agents handle them identically,
/// so the formatting lives here once.
fn append_child_result<H: ClawHttp, Timer: ClawTimer>(
    base: &mut BaseAgent<H, Timer>,
    child: AgentId,
    text: String,
    ok: bool,
) {
    let status = if ok { "ok" } else { "failed" };
    base.push_task_input(format!("[subagent {child} {status}] {text}"));
}

/// Shared: present a non-result child graph event as task input.
fn append_child_input<H: ClawHttp, Timer: ClawTimer>(
    base: &mut BaseAgent<H, Timer>,
    _child: AgentId,
    text: String,
) {
    base.push_task_input(text);
}
