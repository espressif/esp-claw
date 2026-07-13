use claw_interface::http::StreamingHttp;
use claw_interface::{ClawHttp, ClawTimer};

use crate::agent::{AgentAbortHandle, AgentId, BaseAgent, TickOutcome};
use crate::event::EventSink;
use crate::orchestrator::control::DriveControl;
use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};

use super::graph_state::GraphState;

type AgentTickBoxFuture<Http, Timer> = Pin<Box<dyn Future<Output = TickedAgent<Http, Timer>>>>;

pub(super) struct ReadyAgent<Http: ClawHttp, Timer: ClawTimer> {
    pub(super) id: AgentId,
    /// Whether this is the session root. Only the root's iteration events are
    /// forwarded to the session stream; subagents tick with a disabled sink.
    pub(super) is_root: bool,
    pub(super) agent: BaseAgent<Http, Timer>,
}

pub(super) struct TickedAgent<Http: ClawHttp, Timer: ClawTimer> {
    pub(super) id: AgentId,
    pub(super) agent: BaseAgent<Http, Timer>,
    pub(super) outcome: TickOutcome,
}

/// Session-local table of agent ticks currently in flight.
///
/// This is not a batch barrier: polling resolves as soon as any task completes,
/// leaving slower tasks in the table.
pub(super) struct InflightAgentTasks<Http: ClawHttp, Timer: ClawTimer> {
    entries: Vec<Option<InflightAgentTask<Http, Timer>>>,
}

struct InflightAgentTask<Http: ClawHttp, Timer: ClawTimer> {
    id: AgentId,
    is_root: bool,
    abort: AgentAbortHandle,
    future: AgentTickBoxFuture<Http, Timer>,
}

impl<Http: ClawHttp, Timer: ClawTimer> InflightAgentTasks<Http, Timer> {
    pub(super) fn new() -> Self {
        Self {
            entries: Vec::new(),
        }
    }

    pub(super) fn spawn(
        &mut self,
        id: AgentId,
        is_root: bool,
        abort: AgentAbortHandle,
        future: AgentTickBoxFuture<Http, Timer>,
    ) {
        self.entries.push(Some(InflightAgentTask {
            id,
            is_root,
            abort,
            future,
        }));
    }

    pub(super) fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub(super) fn has_root(&self) -> bool {
        self.entries.iter().flatten().any(|entry| entry.is_root)
    }

    pub(super) fn has_background(&self) -> bool {
        self.entries.iter().flatten().any(|entry| !entry.is_root)
    }

    pub(super) fn contains(&self, id: AgentId) -> bool {
        self.entries.iter().flatten().any(|entry| entry.id == id)
    }

    pub(super) fn abort_handles(&self) -> Vec<AgentAbortHandle> {
        self.entries
            .iter()
            .filter_map(|entry| entry.as_ref().map(|entry| entry.abort.clone()))
            .collect()
    }

    pub(super) fn abort_all(&self) {
        for entry in self.entries.iter().flatten() {
            entry.abort.abort();
        }
    }

    /// Cooperative-abort one in-flight agent so a queued graph effect can retask it.
    pub(in crate::orchestrator::instance) fn abort_if_present(&self, id: AgentId) -> bool {
        for entry in &self.entries {
            let Some(entry) = entry else {
                continue;
            };
            if entry.id == id {
                entry.abort.abort();
                return true;
            }
        }
        false
    }

    pub(super) fn retain_live(&mut self, graph: &GraphState) {
        self.entries
            .retain(|entry| entry.as_ref().is_some_and(|entry| graph.contains(entry.id)));
    }

    pub(super) fn next_completed<'a>(
        &'a mut self,
        control: &'a DriveControl,
    ) -> CompletedAgentTicks<'a, Http, Timer> {
        CompletedAgentTicks {
            tasks: self,
            control,
        }
    }
}

pub(super) struct CompletedAgentTicks<'a, Http: ClawHttp, Timer: ClawTimer> {
    tasks: &'a mut InflightAgentTasks<Http, Timer>,
    control: &'a DriveControl,
}

impl<Http: ClawHttp, Timer: ClawTimer> Future for CompletedAgentTicks<'_, Http, Timer> {
    type Output = Vec<TickedAgent<Http, Timer>>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        this.control.set_waker(context.waker().clone());
        if this.control.has_signal() {
            return Poll::Ready(Vec::new());
        }
        let mut completed = Vec::new();
        let mut pending = false;
        for entry_slot in &mut this.tasks.entries {
            let Some(entry) = entry_slot else {
                continue;
            };
            match entry.future.as_mut().poll(context) {
                Poll::Ready(output) => {
                    completed.push(output);
                    *entry_slot = None;
                }
                Poll::Pending => pending = true,
            }
        }
        this.tasks.entries.retain(Option::is_some);
        if !completed.is_empty() {
            Poll::Ready(completed)
        } else if pending {
            Poll::Pending
        } else {
            Poll::Ready(Vec::new())
        }
    }
}

pub(super) fn tick_agent<Http, Timer>(
    ready: ReadyAgent<Http, Timer>,
    events: EventSink,
) -> AgentTickBoxFuture<Http, Timer>
where
    Http: ClawHttp + StreamingHttp + 'static,
    Timer: ClawTimer + 'static,
{
    Box::pin(async move {
        let ReadyAgent {
            id,
            is_root,
            mut agent,
            ..
        } = ready;
        let outcome = agent.tick(&events).await;
        match &outcome {
            TickOutcome::AwaitingApproval { .. } => {
                tracing::info!(name: "awaiting_approval", agent = %id);
            }
            TickOutcome::Cancelled => {
                if is_root {
                    tracing::warn!(name: "root_cancelled", "");
                } else {
                    tracing::warn!(name: "subagent_cancelled", agent = %id);
                }
            }
            TickOutcome::Failed(_) => {
                tracing::error!(name: "task_failed", "");
            }
            _ => {}
        }
        TickedAgent { id, agent, outcome }
    })
}
