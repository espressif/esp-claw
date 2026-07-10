use claw_context::{Block, BlockKind, Context};
use claw_interface::{ClawHttp, ClawTimer};
use claw_tool::ToolGate;
use serde_json::Value;
use tracing::Instrument as _;

use crate::event::EventSink;
use crate::memory::{ContextAdapter, ContextAdapterInput};

use super::control::PermissionGate;
use super::state::AgentLifecycle;
use super::{BaseAgent, BaseAgentBuildError, TickOutcome};
use crate::agent::iteration_loop::{
    ChatMessages, IterationId, IterationLoop, IterationResult, IterationStep, SystemPrompt,
};

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    /// Process queued commands, advance at most one iteration, and report what
    /// happened as a [`TickOutcome`].
    pub async fn tick(&mut self, events: &EventSink) -> TickOutcome {
        self.outcome = None;

        self.drain_inbox();

        if self.state.get().lifecycle == AgentLifecycle::Running {
            let iteration_id = self.state.get_mut().iterations.next();
            let outcome = self.run_iteration(iteration_id, events).await;
            self.reduce_outcome(outcome);
            self.drain_control_signals();
            self.drain_inbox();
        }

        let lifecycle = self.state.get().lifecycle;
        if self.state.get().projected_lifecycle != lifecycle {
            self.state.get_mut().projected_lifecycle = lifecycle;
        }

        match self.outcome.take() {
            Some(outcome) => outcome,
            None => match self.state.get().lifecycle {
                AgentLifecycle::Running => TickOutcome::Working,
                _ => TickOutcome::Idle,
            },
        }
    }

    /// Register a pluggable [`ContextAdapter`].
    pub fn register_context_adapter(
        &mut self,
        adapter: Box<dyn ContextAdapter>,
    ) -> Result<(), BaseAgentBuildError> {
        for tool in adapter.tools() {
            self.tools.add_tool(tool)?;
        }
        self.adapters.push(adapter);
        Ok(())
    }

    async fn prepare_adapter_context(
        adapters: &mut [Box<dyn ContextAdapter>],
        history_view: &dyn crate::memory::History,
    ) {
        for adapter in adapters {
            adapter
                .prepare(ContextAdapterInput {
                    history: history_view,
                })
                .await;
        }
    }

    fn render_adapter_context(
        adapters: &mut [Box<dyn ContextAdapter>],
        history_view: &dyn crate::memory::History,
        context: &mut Context,
    ) -> Value {
        let mut sink = context.sink();
        for adapter in adapters {
            adapter.contribute(
                ContextAdapterInput {
                    history: history_view,
                },
                &mut sink,
            );
        }
        sink.into_history()
    }

    pub(super) async fn run_iteration(
        &mut self,
        iteration_id: IterationId,
        events: &EventSink,
    ) -> IterationResult {
        // Context adapters may perform auxiliary LLM work (conversation
        // compaction and long-term-memory extraction). Keep that work in a
        // distinct bracket so it cannot disappear into the parent `agent` span
        // before the user-facing iteration begins.
        let adapter_count = self.adapters.len() as u64;
        let prepare_span = tracing::info_span!(
            "iteration.prepare",
            run.iteration = %iteration_id,
            adapter_count,
        );
        let tools = prepare_span.in_scope(|| self.tools.begin())?;
        let history_view = self.transcript.as_history();
        Self::prepare_adapter_context(&mut self.adapters, history_view)
            .instrument(prepare_span.clone())
            .await;
        prepare_span.in_scope(|| {
            self.context
                .with(Block::new(BlockKind::ToolPolicy, tools.tool_context()))
                .with_reminder(BlockKind::ToolReminder, Some(tools.extra_tool_context()));
        });

        let render_span =
            prepare_span.in_scope(|| tracing::info_span!("context.render", adapter_count));
        let history = render_span.in_scope(|| {
            Self::render_adapter_context(&mut self.adapters, history_view, &mut self.context)
        });
        let iteration_loop = IterationLoop {
            llm: &mut self.llm,
            interruption: &self.interruption,
            retry: self.retry_policy,
            events,
        };
        let state = self.state.get();
        let permission_gate = PermissionGate {
            policy: self.permission_policy.as_ref(),
            grants: &state.permission_grants,
        };
        let gate = &permission_gate as &dyn ToolGate;
        let context = render_span.in_scope(|| self.context.request(&history));
        let step = IterationStep {
            iteration_id,
            system_prompt: SystemPrompt(context.system()),
            messages: ChatMessages(context.history()),
            reminders: context.reminders(),
            tools: &tools,
            gate,
        };
        drop(render_span);
        drop(prepare_span);
        iteration_loop.run(step).await
    }
}
