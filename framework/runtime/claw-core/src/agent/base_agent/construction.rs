use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use claw_api::{ClawApiAsync, ClawApiConfig, InitError, RetryPolicy};
use claw_checkpoint::DurableState;
use claw_context::{Block, Context};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::TranscriptStore;
use claw_skill::SkillSet;
use claw_tool::ToolSet;

use crate::agent::tools::{internal_tools, ControlSink};
use crate::memory::{ContextAdapter, SkillContextAdapter, Transcript};

use super::control::AgentInterruption;
use super::state::BaseAgentState;
use super::{BaseAgent, BaseAgentBuildError};

/// All construction-time configuration for a [`BaseAgent`], consumed by
/// [`BaseAgent::build`].
pub(in crate::agent) struct BaseAgentConfig<F: ClawFs + 'static> {
    pub(in crate::agent) store: TranscriptStore<F>,
    pub(in crate::agent) tools: ToolSet,
    pub(in crate::agent) skills: SkillSet,
    pub(in crate::agent) agent_instruction: Block<'static>,
    pub(in crate::agent) inherited_context: Vec<Block<'static>>,
    pub(in crate::agent) retry_policy: RetryPolicy,
    pub(in crate::agent) block_retries: u32,
}

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    /// Rebind the agent's LLM client to `config` for the next turn, keeping the
    /// existing transport (see [`ClawApiAsync::set_config`]).
    pub(crate) fn set_llm_config(&mut self, config: ClawApiConfig) -> Result<(), InitError> {
        self.llm.set_config(config)
    }

    pub(crate) fn set_context_block(&mut self, block: Block<'static>) {
        self.context.with(block);
    }

    /// Assemble a runnable agent from a [`BaseAgentConfig`].
    pub(in crate::agent) fn build<F: ClawFs + 'static>(
        config: BaseAgentConfig<F>,
    ) -> Result<BaseAgent<H, Timer>, BaseAgentBuildError>
    where
        H: Default,
        Timer: Default,
    {
        let control: ControlSink = Arc::new(Mutex::new(VecDeque::new()));

        let llm = ClawApiAsync::<H, Timer>::new(H::default(), Timer::default());

        let mut tools = config.tools;
        tools.add_group(internal_tools(Arc::clone(&control)))?;

        let mut context = Context::new();
        for block in config.inherited_context {
            context.with(block);
        }
        context.with(config.agent_instruction);

        let skill_adapter: Box<dyn ContextAdapter> =
            Box::new(SkillContextAdapter::new(config.skills));
        let transcript: Box<dyn Transcript> = Box::new(config.store);
        if let Some(group) = skill_adapter.tools() {
            tools.add_group(group)?;
        }
        let adapters = vec![skill_adapter];

        Ok(BaseAgent {
            llm,
            retry_policy: config.retry_policy,
            interruption: AgentInterruption::new(),
            transcript,
            tools,
            context,
            state: DurableState::new(BaseAgentState::new(config.block_retries)),
            outcome: None,
            control,
            adapters,
        })
    }
}
