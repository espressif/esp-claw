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

use claw_api::ClawApiAsync;
use claw_context::Block;
use claw_interface::{ClawFs, ClawHttpAsync, ClawTimer};
use claw_memory::{Compactor, TranscriptConfig, TranscriptStore};
use claw_utils::SharedTaskPool;

use crate::agent::base_agent::{
    AgentCommand, AgentCommandError, AgentId, BaseAgent, BaseAgentBuildError,
};
use crate::agent::config::AgentConfig;
use crate::agent::graph::{AgentContext, GraphHost};
use crate::agent::kind::AgentKind;
use crate::agent::tools::{respond_to_approval_tool_group, subagent_tool_group};
use crate::agent::{append_child_result, Agent, AgentTickFuture};
use crate::memory::{
    CompactionPolicy, ContextAdapter, History, RollingSummaryContextAdapter, SummaryCursor,
};
use claw_tool::{ToolSet, ToolSetError};

/// The compaction collaborators a [`GenericAgent`] hands to its rolling-summary
/// adapter: the shared worker pool the summarization runs on, the summarizer
/// itself, and the policy budgets. Bundled so the construction signature and the
/// factory's per-agent clone stay small.
///
/// These belong to the *agent layer*, not the transcript store: the store is pure
/// verbatim storage and never compacts (see [`TranscriptStore`]). The summarizer
/// seam stays dynamic (`Arc<dyn Compactor>`) — swapped per build, not a hot path.
pub struct CompactionDeps {
    /// Shared worker pool the summarization runs on, off the tick path.
    pub pool: Arc<SharedTaskPool>,
    /// How aged windows are turned into a summary.
    pub compactor: Arc<dyn Compactor>,
    /// When/what to compact.
    pub policy: CompactionPolicy,
}

// ===========================================================================
// GenericAgent: the flat Agent over BaseAgent
// ===========================================================================

/// The one agent type: a flat ReAct loop over a [`BaseAgent`], configured by an
/// [`AgentConfig`]. No semantic FSM — `tick` forwards straight to the base.
pub struct GenericAgent<H: ClawHttpAsync, Timer: ClawTimer> {
    id: AgentId,
    kind: AgentKind,
    base: BaseAgent<H, Timer>,
}

impl<H: ClawHttpAsync, Timer: ClawTimer> GenericAgent<H, Timer> {
    /// Build a generic agent with `id` over `llm`, configured by `config`.
    ///
    /// The agent constructs its **own** [`TranscriptStore`] from the injected
    /// `transcript_config` (base dir + tuning) and `fs`, keyed by its own `id`.
    /// Keeping the store construction inside the agent means a caller cannot wire
    /// a transcript that belongs to a different agent: the conversation identity
    /// always follows the agent identity.
    ///
    /// `compaction` (pool + compactor + policy) belongs to the agent layer, not
    /// the store: it drives the [`RollingSummaryContextAdapter`], which summarizes
    /// the aged prefix at request time. That adapter and the always-present
    /// recent-history adapter share one [`SummaryCursor`] — the boundary between
    /// the summarized prefix and the verbatim tail — so this one agent owns both
    /// halves of its history with no gap or overlap.
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
    pub fn new<F: ClawFs + 'static>(
        id: AgentId,
        llm: ClawApiAsync<H, Timer>,
        transcript_config: TranscriptConfig,
        fs: F,
        compaction: CompactionDeps,
        config: AgentConfig,
        host: Option<Arc<dyn GraphHost>>,
        is_root: bool,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Self, GenericAgentBuildError> {
        // Memory identity follows agent identity: the conversation is keyed by the
        // agent's own id, so the transcript can never be mismatched by the caller.
        let store = TranscriptStore::new(id.0, transcript_config, fs);

        // The two conversation adapters share one cursor: the rolling summary
        // advances it as it summarizes; the recent-tail adapter (wired by the base
        // builder below) renders only the turns past it.
        let cursor = SummaryCursor::new();
        let rolling_summary = RollingSummaryContextAdapter::new(
            store.clone(),
            compaction.pool,
            compaction.compactor,
            compaction.policy,
            cursor.clone(),
        );

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

        let mut base_builder = BaseAgent::builder(llm, store)
            .with_system_prompt(config.system_prompt)
            .with_tools(tool_set)
            .with_inherited_context(inherited_context)
            .with_retry_policy(config.retry_policy)
            .with_summary_cursor(cursor);
        // The soft-hide "retry then fail" budget is the agent's BlockPolicy.
        if let Some(retries) = config.tool_block_retries {
            base_builder = base_builder.with_block_retries(retries);
        }
        if let Some(skills) = config.skills {
            base_builder = base_builder.with_skills(skills);
        }
        let mut base = base_builder.build()?;
        // Attach the rolling compact-summary half of the history. The base agent
        // already wired the recent verbatim half; this one carries the compaction
        // policy the builder lacks, so the agent layer registers it here.
        base.register_context_adapter(Box::new(rolling_summary))?;

        Ok(Self {
            id,
            kind: config.kind,
            base,
        })
    }

    /// This agent's kind.
    pub fn kind(&self) -> &AgentKind {
        &self.kind
    }

    /// A read-only view of this agent's conversation transcript, as the narrow
    /// [`History`] capability — for inspection (CLI, tests), never mutation.
    ///
    /// Delegates to [`BaseAgent::history`]: the concrete conversation-memory
    /// type and its filesystem parameter stay hidden, which is why this agent is
    /// not generic over a filesystem. A reader depends only on [`History`], the
    /// same boundary every other caller uses.
    pub fn history(&self) -> &dyn History {
        self.base.history()
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

impl<H: ClawHttpAsync + Send, Timer: ClawTimer + Send> Agent for GenericAgent<H, Timer> {
    fn id(&self) -> AgentId {
        self.id
    }

    fn send_command(&mut self, command: AgentCommand) -> Result<(), AgentCommandError> {
        self.base.send_command(command)
    }

    fn deliver_child_result(&mut self, child: AgentId, text: String, ok: bool) {
        append_child_result(&mut self.base, child, text, ok);
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
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::{Arc, Mutex};
    use std::task::{Wake, Waker};

    use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig, RetryPolicy};
    use claw_interface::{
        BlockingClawHttpAsync, ClawHttpAsync, ClawTimer, ImmediateTimer, MemFs, ScriptedHttp,
        StdThread,
    };
    use claw_memory::NoopCompactor;
    use claw_utils::{PoolConfig, SharedTaskPool};
    use serde_json::{json, Value};

    use super::*;
    use crate::agent::graph::{GraphEffect, SpawnPolicy};
    use crate::agent::TickOutcome;

    type TestLlm = ClawApiAsync<BlockingClawHttpAsync<ScriptedHttp>, ImmediateTimer>;

    fn scripted_llm(bodies: Vec<String>) -> TestLlm {
        let config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        );
        ClawApiAsync::init(
            config,
            BlockingClawHttpAsync::new(ScriptedHttp::new(bodies)),
            ImmediateTimer,
        )
        .expect("init llm")
    }

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    /// The ingredients [`GenericAgent::new`] needs to build its own transcript: a
    /// base config, the storage backend, and the (in-memory) compaction
    /// collaborators. The agent keys the conversation by its own id.
    fn memory_ingredients(agent_id: AgentId) -> (TranscriptConfig, MemFs, CompactionDeps) {
        let pool =
            Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread).expect("memory pool"));
        (
            TranscriptConfig::new(format!("/mem/agent-{}", agent_id.0)),
            MemFs::default(),
            CompactionDeps {
                pool,
                compactor: Arc::new(NoopCompactor),
                policy: CompactionPolicy::new(6000, 2000, 1500),
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

    fn transcript_contents(history: &dyn History) -> Vec<String> {
        history
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

    fn drive<H: ClawHttpAsync + Send, Timer: ClawTimer + Send>(
        agent: &mut GenericAgent<H, Timer>,
    ) -> TickOutcome {
        block_on(async {
            loop {
                match agent.tick().await {
                    TickOutcome::Working => continue,
                    other => return other,
                }
            }
        })
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
        let (mem_config, fs, compaction) = memory_ingredients(AgentId(1));
        let mut config = test_config("conversation");
        config.system_prompt = "be helpful".into();
        let mut agent = GenericAgent::new(
            AgentId(1),
            scripted_llm(vec![body_plain_text("hi")]),
            mem_config,
            fs,
            compaction,
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
        let (mem_config, fs, compaction) = memory_ingredients(AgentId(1));
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
            fs,
            compaction,
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
        let (mem_config, fs, compaction) = memory_ingredients(AgentId(1));
        let config = test_config("conversation");
        let mut agent = GenericAgent::new(
            AgentId(1),
            scripted_llm(vec![body_plain_text("ack")]),
            mem_config,
            fs,
            compaction,
            config,
            noop_host(),
            false,
            Arc::from([]),
        )
        .unwrap();

        agent.deliver_child_result(AgentId(7), "subtask output".into(), true);
        let _ = drive(&mut agent);

        assert!(transcript_contents(agent.history())
            .iter()
            .any(|c| c.contains("[subagent agent-7 ok] subtask output")));
    }

    #[test]
    fn respond_to_approval_tool_is_wired_for_a_root() {
        use crate::agent::graph::ApprovalVerdict;

        let host = Arc::new(RecordingHost::default());
        let (mem_config, fs, compaction) = memory_ingredients(AgentId(1));
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
            fs,
            compaction,
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
