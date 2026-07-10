use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use claw_api::{ClawApiAsync, ClawApiConfig, RetryPolicy};
use claw_checkpoint::DurableState;
use claw_context::{Block, Context};
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::TranscriptStore;
use claw_permission::PermissionPolicy;
use claw_skill::SkillSet;
use claw_tool::ToolSet;

use crate::agent::manifest::RetryCount;
use crate::agent::tools::{internal_tools, ControlSink};
use crate::memory::{ContextAdapter, SkillContextAdapter, Transcript};

use super::control::AgentInterruption;
use super::state::BaseAgentState;
use super::{BaseAgent, BaseAgentBuildError};

/// All construction-time configuration for a [`BaseAgent`], consumed by
/// [`BaseAgent::build`].
pub(crate) struct BaseAgentConfig<F: ClawFs + 'static> {
    pub llm_config: ClawApiConfig,
    pub store: TranscriptStore<F>,
    pub tools: ToolSet,
    pub skills: SkillSet,
    pub agent_instruction: Block<'static>,
    pub inherited_context: Arc<[Block<'static>]>,
    pub retry_policy: RetryPolicy,
    pub permission_policy: Arc<dyn PermissionPolicy>,
    pub block_retries: RetryCount,
}

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    /// Assemble a runnable agent from a [`BaseAgentConfig`].
    pub fn build<F: ClawFs + 'static>(
        config: BaseAgentConfig<F>,
    ) -> Result<BaseAgent<H, Timer>, BaseAgentBuildError>
    where
        H: Default,
        Timer: Default,
    {
        let control: ControlSink = Arc::new(Mutex::new(VecDeque::new()));

        let llm = ClawApiAsync::<H, Timer>::init_default(config.llm_config)?;

        let mut tools = config.tools;
        for tool in internal_tools(Arc::clone(&control)) {
            tools.add_tool(tool)?;
        }

        let permission_policy = config.permission_policy;

        let mut context = Context::new();
        for block in config.inherited_context.iter() {
            context.with(block.clone());
        }
        context.with(config.agent_instruction);

        let skill_adapter: Box<dyn ContextAdapter> =
            Box::new(SkillContextAdapter::new(config.skills));
        let transcript: Box<dyn Transcript> = Box::new(config.store);
        for tool in skill_adapter.tools() {
            tools.add_tool(tool)?;
        }
        let adapters = vec![skill_adapter];

        Ok(BaseAgent {
            llm,
            retry_policy: config.retry_policy,
            interruption: AgentInterruption::new(),
            transcript,
            tools,
            permission_policy,
            context,
            state: DurableState::new(BaseAgentState::new(config.block_retries.get())),
            outcome: None,
            control,
            adapters,
        })
    }
}
