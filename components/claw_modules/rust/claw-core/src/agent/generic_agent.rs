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
//! Where an [`AgentConfig`] comes from — a compile-time-baked agent manifest
//! resolved by kind through an [`AgentResolver`] — lives in the crate-internal
//! `manifest` module. This module only consumes the resolved config.

use std::sync::Arc;

use claw_api::ClawApi;
use claw_context::Block;
use claw_interface::http::ClawHttp;
use claw_interface::ClawFs;
use claw_memory::{ConversationConfig, ConversationDeps, ConversationMemory};

use crate::agent::base_agent::{
    AgentCommand, AgentCommandError, AgentId, BaseAgent, BaseAgentBuildError, TickOutcome,
};
use crate::agent::config::AgentConfig;
use crate::agent::graph::{AgentContext, GraphHost};
use crate::agent::kind::AgentKind;
use crate::agent::tools::{respond_to_approval_tool_group, subagent_tool_group};
use crate::agent::{append_child_result, Agent};
use crate::memory::Memory;
use claw_tool::{ToolSet, ToolSetError};

// ===========================================================================
// GenericAgent: the flat Agent over BaseAgent
// ===========================================================================

/// The one agent type: a flat ReAct loop over a [`BaseAgent`], configured by an
/// [`AgentConfig`]. No semantic FSM — `tick` forwards straight to the base.
pub struct GenericAgent<F: ClawFs + 'static, H: ClawHttp> {
    id: AgentId,
    kind: AgentKind,
    /// A view sharing inner state with the base agent's memory, for reads and
    /// persistence inspection (a cheap `Arc` clone of the live transcript).
    memory: ConversationMemory<F>,
    base: BaseAgent<H>,
}

impl<F: ClawFs + 'static, H: ClawHttp> GenericAgent<F, H> {
    /// Build a generic agent with `id` over `llm`, configured by `config`.
    ///
    /// The agent constructs its **own** [`ConversationMemory`] from the injected
    /// `memory_config` (base dir + tuning) and `memory_deps` (fs/pool/compactor),
    /// keyed by its own `id`. Keeping memory construction inside the agent means a
    /// caller cannot wire a transcript that belongs to a different agent: the
    /// conversation identity always follows the agent identity.
    ///
    /// The config's capability tools are merged with the graph tools that require
    /// a [`GraphHost`]: `spawn_subagent` when `config.spawn_enabled`, and
    /// `respond_to_approval` when `is_root`. With no `host` (a standalone agent,
    /// no graph) neither is attached. The base agent then adds its built-in
    /// self-control tool (`end_conversation`).
    ///
    /// `inherited_context` is the scope-layered prose injected from above
    /// (Global -> Session), handed straight to the base agent so it renders ahead
    /// of the agent's own blocks. Empty for a standalone agent.
    ///
    /// # Errors
    ///
    /// [`GenericAgentBuildError`] when tool assembly or base construction fails.
    pub fn new(
        id: AgentId,
        llm: ClawApi<H>,
        memory_config: ConversationConfig,
        memory_deps: ConversationDeps<F>,
        config: AgentConfig,
        host: Option<Arc<dyn GraphHost>>,
        is_root: bool,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Self, GenericAgentBuildError> {
        // Memory identity follows agent identity: the conversation is keyed by the
        // agent's own id, so the transcript can never be mismatched by the caller.
        let memory = ConversationMemory::new(id.0, memory_config, memory_deps);
        let memory_view = memory.clone();

        let mut tool_set = ToolSet::new(config.tools)?;
        // Graph-affecting tools need a back-channel; without one (standalone agent)
        // the agent simply has no spawn/approval-routing tools.
        if let Some(host) = host {
            let context = Arc::new(AgentContext::new(id, host));
            if config.spawn_enabled {
                tool_set.extend_with_group(subagent_tool_group(
                    Arc::clone(&context),
                    config.spawn_policy,
                ))?;
            }
            if is_root {
                tool_set.extend_with_group(respond_to_approval_tool_group(context))?;
            }
        }

        let mut base_builder = BaseAgent::builder(llm, memory)
            .with_system_prompt(config.system_prompt)
            .with_tools(tool_set)
            .with_inherited_context(inherited_context)
            .with_retry_policy(config.retry_policy);
        // The soft-hide "retry then fail" budget is the agent's BlockPolicy.
        if let Some(retries) = config.tool_block_retries {
            base_builder = base_builder.with_block_retries(retries);
        }
        if let Some(skills) = config.skills {
            base_builder = base_builder.with_skills(skills);
        }
        let base = base_builder.build()?;

        Ok(Self {
            id,
            kind: config.kind,
            memory: memory_view,
            base,
        })
    }

    /// This agent's kind.
    pub fn kind(&self) -> &AgentKind {
        &self.kind
    }

    /// A read-only view of this agent's conversation memory.
    ///
    /// Shares inner state with the live agent (cheap `Arc` clone), so reads always
    /// reflect the current transcript. Intended for inspection and persistence,
    /// not direct mutation (the agent owns writes through its tick loop).
    pub fn memory(&self) -> &ConversationMemory<F> {
        &self.memory
    }

    /// Register a pluggable long-term [`Memory`] on the underlying base agent.
    ///
    /// Forwards to [`BaseAgent::register_memory`]; the factory calls this after
    /// construction to attach the dual-tier long-term store.
    ///
    /// # Errors
    ///
    /// [`BaseAgentBuildError`] when a memory tool clashes with an existing tool or
    /// the LLM does not support the tools the memory provides.
    pub fn register_memory(&mut self, memory: Arc<dyn Memory>) -> Result<(), BaseAgentBuildError> {
        self.base.register_memory(memory)
    }
}

impl<F: ClawFs + 'static, H: ClawHttp + Send> Agent for GenericAgent<F, H> {
    fn id(&self) -> AgentId {
        self.id
    }

    fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        self.base.send_command(command)
    }

    fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool) {
        append_child_result(&mut self.base, child, text, ok);
    }

    fn tick(&mut self) -> TickOutcome {
        // Flat: no FSM, no per-phase gating — the model drives its own flow.
        self.base.tick()
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
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use std::sync::{Arc, Mutex};

    use claw_api::{ClawApi, ClawApiConfig, RetryPolicy};
    use claw_interface::{MemFs, ScriptedHttp, StdThread};
    use claw_memory::{ConversationConfig, ConversationDeps, ConversationMemory, NoopCompactor};
    use claw_utils::{PoolConfig, SharedTaskPool};
    use serde_json::{json, Value};

    use super::*;
    use crate::agent::graph::{GraphEffect, SpawnPolicy};

    fn scripted_llm(bodies: Vec<String>) -> ClawApi<ScriptedHttp> {
        ClawApi::init(
            ClawApiConfig {
                api_key: Some("sk-test".into()),
                backend_type: "openai_compatible".into(),
                model: Some("gpt-test".into()),
                base_url: Some("https://example.invalid".into()),
                supports_tools: true,
                ..Default::default()
            },
            ScriptedHttp::new(bodies),
        )
        .expect("init llm")
    }

    /// The ingredients [`GenericAgent::new`] needs to build its own memory: a
    /// base config plus the in-memory collaborators. The agent keys the
    /// conversation by its own id.
    fn memory_ingredients(agent_id: AgentId) -> (ConversationConfig, ConversationDeps<MemFs>) {
        let pool =
            Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread).expect("memory pool"));
        (
            ConversationConfig::new(format!("/mem/agent-{}", agent_id.0)),
            ConversationDeps {
                fs: MemFs::default(),
                pool,
                compactor: Arc::new(NoopCompactor),
            },
        )
    }

    fn body_plain_text(text: &str) -> String {
        json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
    }

    fn body_tool_call(id: &str, name: &str, arguments_json: &str) -> String {
        json!({
            "choices": [{
                "message": {
                    "role": "assistant",
                    "tool_calls": [{
                        "id": id,
                        "function": { "name": name, "arguments": arguments_json }
                    }]
                }
            }]
        })
        .to_string()
    }

    fn transcript_contents(memory: &ConversationMemory<MemFs>) -> Vec<String> {
        memory
            .messages()
            .as_array()
            .map(|items| {
                items
                    .iter()
                    .filter_map(|m| m.get("content").and_then(Value::as_str))
                    .map(str::to_string)
                    .collect()
            })
            .unwrap_or_default()
    }

    fn drive<H: ClawHttp + Send>(agent: &mut GenericAgent<MemFs, H>) -> TickOutcome {
        loop {
            match agent.tick() {
                TickOutcome::Working => continue,
                other => return other,
            }
        }
    }

    /// A bare config for `kind` with no tools/skills — the in-crate test seam now
    /// that configs are otherwise built only from baked manifests. Fields are
    /// `pub(in crate::agent)`, so tests construct the struct directly.
    fn test_config(kind: &str) -> AgentConfig {
        AgentConfig {
            kind: AgentKind::new(kind),
            system_prompt: String::new(),
            tools: Vec::new(),
            skills: None,
            spawn_enabled: false,
            spawn_policy: SpawnPolicy::Any,
            retry_policy: RetryPolicy::default(),
            tool_block_retries: None,
        }
    }

    /// A [`GraphHost`] that records every emitted effect and hands out ascending
    /// ids; doubles as a no-op host for the single-agent tests.
    #[derive(Default)]
    struct RecordingHost {
        next: Mutex<usize>,
        effects: Mutex<Vec<(AgentId, GraphEffect)>>,
    }

    impl GraphHost for RecordingHost {
        fn next_id(&self) -> AgentId {
            let mut next = self.next.lock().unwrap();
            *next += 1;
            AgentId(*next)
        }
        fn emit(&self, requester: AgentId, effect: GraphEffect) {
            self.effects.lock().unwrap().push((requester, effect));
        }
        fn snapshot(&self) -> Vec<crate::agent::graph::AgentSnapshot> {
            Vec::new()
        }
    }

    fn noop_host() -> Option<Arc<dyn GraphHost>> {
        Some(Arc::new(RecordingHost::default()))
    }

    #[test]
    fn flat_agent_answers_directly() {
        let (mem_config, mem_deps) = memory_ingredients(AgentId(1));
        let mut config = test_config("conversation");
        config.system_prompt = "be helpful".into();
        let mut agent = GenericAgent::new(
            AgentId(1),
            scripted_llm(vec![body_plain_text("hi")]),
            mem_config,
            mem_deps,
            config,
            noop_host(),
            false,
            Arc::from([]),
        )
        .unwrap();

        agent
            .send_command(AgentCommand::AppendMessage("hello".into()))
            .unwrap();
        match drive(&mut agent) {
            TickOutcome::Yielded { text } => assert_eq!(text, "hi"),
            other => panic!("unexpected: {other:?}"),
        }
        assert_eq!(agent.kind().as_str(), "conversation");
    }

    #[test]
    fn end_conversation_ends_the_task() {
        let (mem_config, mem_deps) = memory_ingredients(AgentId(1));
        let body = body_tool_call(
            "e1",
            "end_conversation",
            &json!({ "final_message": "bye" }).to_string(),
        );
        let config = test_config("worker");
        let mut agent = GenericAgent::new(
            AgentId(1),
            scripted_llm(vec![body]),
            mem_config,
            mem_deps,
            config,
            noop_host(),
            false,
            Arc::from([]),
        )
        .unwrap();

        agent
            .send_command(AgentCommand::AppendMessage("go".into()))
            .unwrap();
        match drive(&mut agent) {
            TickOutcome::Ended { final_message } => assert_eq!(final_message, "bye"),
            other => panic!("unexpected: {other:?}"),
        }
    }

    #[test]
    fn child_result_is_appended_as_message() {
        let (mem_config, mem_deps) = memory_ingredients(AgentId(1));
        let config = test_config("conversation");
        let mut agent = GenericAgent::new(
            AgentId(1),
            scripted_llm(vec![body_plain_text("ack")]),
            mem_config,
            mem_deps,
            config,
            noop_host(),
            false,
            Arc::from([]),
        )
        .unwrap();

        agent.deliver_child_result(AgentId(7), "subtask output".into(), true);
        let _ = drive(&mut agent);

        assert!(transcript_contents(agent.memory())
            .iter()
            .any(|c| c.contains("[subagent agent-7 ok] subtask output")));
    }

    #[test]
    fn respond_to_approval_tool_is_wired_for_a_root() {
        use crate::agent::graph::ApprovalVerdict;

        let host = Arc::new(RecordingHost::default());
        let (mem_config, mem_deps) = memory_ingredients(AgentId(1));
        let config = test_config("conversation");
        // A root (is_root = true) with a graph host gets the `respond_to_approval`
        // tool; calling it emits a `ResolveApproval` effect on the host.
        let mut agent = GenericAgent::new(
            AgentId(1),
            scripted_llm(vec![
                body_tool_call(
                    "a1",
                    "respond_to_approval",
                    &json!({ "agent": "agent-7", "verdict": "no", "note": "too risky" })
                        .to_string(),
                ),
                body_plain_text("handled"),
            ]),
            mem_config,
            mem_deps,
            config,
            Some(Arc::clone(&host) as Arc<dyn GraphHost>),
            true,
            Arc::from([]),
        )
        .unwrap();

        agent
            .send_command(AgentCommand::AppendMessage("the user said no".into()))
            .unwrap();
        let _ = drive(&mut agent);

        let effects = host.effects.lock().unwrap();
        assert_eq!(
            effects.as_slice(),
            &[(
                AgentId(1),
                GraphEffect::ResolveApproval {
                    target: AgentId(7),
                    verdict: ApprovalVerdict::No,
                    note: Some("too risky".to_string()),
                }
            )]
        );
    }
}
