use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::{btree_map::Entry, BTreeMap, VecDeque};

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawHttp, ClawTimer};

use crate::agent::{AgentAbortHandle, BaseAgent, TickOutcome};
use crate::protocol::{AgentId, EventSink, Message, TurnOrigin};

use super::drive_control::DriveControl;
use super::model::{SubagentResult, TranscriptText};
use super::tool_port::MultiagentBridge;

type AgentTickFuture<Http, Timer> = Pin<Box<dyn Future<Output = TickedAgent<Http, Timer>>>>;

/// Whether a slot is idle or currently running one agent tick.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum AgentAvailability {
    Available,
    InFlight,
}

pub(super) struct ReadyAgent<Http: ClawHttp, Timer: ClawTimer> {
    pub(super) id: AgentId,
    /// Only the root's iteration events are forwarded to the session stream.
    pub(super) is_root: bool,
    pub(super) agent: BaseAgent<Http, Timer>,
}

pub(super) struct CompletedAgentTick {
    pub(super) id: AgentId,
    pub(super) outcome: TickOutcome,
}

pub(super) struct TickedAgent<Http: ClawHttp, Timer: ClawTimer> {
    agent: BaseAgent<Http, Timer>,
    outcome: TickOutcome,
}

struct RunningAgent<Http: ClawHttp, Timer: ClawTimer> {
    is_root: bool,
    abort: AgentAbortHandle,
    future: AgentTickFuture<Http, Timer>,
}

enum AgentExecution<Http: ClawHttp, Timer: ClawTimer> {
    Idle(BaseAgent<Http, Timer>),
    Running(RunningAgent<Http, Timer>),
}

/// Read-only projection used by checkpoint encoding.
pub(super) struct AgentSlotView<'a, Http: ClawHttp, Timer: ClawTimer> {
    id: AgentId,
    agent: Option<&'a BaseAgent<Http, Timer>>,
    inbox: &'a VecDeque<Message>,
}

impl<'a, Http: ClawHttp, Timer: ClawTimer> AgentSlotView<'a, Http, Timer> {
    pub(super) fn id(&self) -> AgentId {
        self.id
    }

    pub(super) fn agent(&self) -> Option<&'a BaseAgent<Http, Timer>> {
        self.agent
    }

    pub(super) fn inbox(&self) -> &'a VecDeque<Message> {
        self.inbox
    }
}

/// Stable storage for one live graph node.
///
/// The slot owns both forms of the same agent: either the idle `BaseAgent`, or
/// the future currently ticking it. Its inbox remains available in both states.
struct AgentSlot<Http: ClawHttp, Timer: ClawTimer> {
    execution: Option<AgentExecution<Http, Timer>>,
    inbox: VecDeque<Message>,
}

impl<Http: ClawHttp, Timer: ClawTimer> AgentSlot<Http, Timer> {
    fn new(agent: BaseAgent<Http, Timer>) -> Self {
        Self {
            execution: Some(AgentExecution::Idle(agent)),
            inbox: VecDeque::new(),
        }
    }

    fn idle_agent(&self) -> Option<&BaseAgent<Http, Timer>> {
        match self.execution.as_ref()? {
            AgentExecution::Idle(agent) => Some(agent),
            AgentExecution::Running(_) => None,
        }
    }

    fn idle_agent_mut(&mut self) -> Option<&mut BaseAgent<Http, Timer>> {
        match self.execution.as_mut()? {
            AgentExecution::Idle(agent) => Some(agent),
            AgentExecution::Running(_) => None,
        }
    }

    fn take_idle(&mut self) -> Option<BaseAgent<Http, Timer>> {
        match self.execution.take()? {
            AgentExecution::Idle(agent) => Some(agent),
            running @ AgentExecution::Running(_) => {
                self.execution = Some(running);
                None
            }
        }
    }

    fn start(
        &mut self,
        id: AgentId,
        is_root: bool,
        abort: AgentAbortHandle,
        future: AgentTickFuture<Http, Timer>,
    ) {
        assert!(
            self.execution
                .replace(AgentExecution::Running(RunningAgent {
                    is_root,
                    abort,
                    future,
                }))
                .is_none(),
            "agent slot must be checked out before it starts: {id}"
        );
    }

    fn is_running(&self) -> bool {
        matches!(self.execution, Some(AgentExecution::Running(_)))
    }

    fn is_running_root(&self) -> bool {
        matches!(
            self.execution,
            Some(AgentExecution::Running(RunningAgent { is_root: true, .. }))
        )
    }

    fn abort_handle(&self) -> Option<AgentAbortHandle> {
        match self.execution.as_ref()? {
            AgentExecution::Idle(agent) => Some(agent.abort_handle()),
            AgentExecution::Running(running) => Some(running.abort.clone()),
        }
    }

    fn abort_if_running(&self) -> bool {
        let Some(AgentExecution::Running(running)) = &self.execution else {
            return false;
        };
        running.abort.abort();
        true
    }

    fn activate_next_message(&mut self) -> bool {
        let Some(message) = self.inbox.pop_front() else {
            return false;
        };
        let Some(agent) = self.idle_agent_mut() else {
            self.inbox.push_front(message);
            return false;
        };
        agent.activate_deferred_message(message);
        true
    }

    fn deliver_child_result(&mut self, result: SubagentResult) -> AgentAvailability {
        self.inbox
            .push_back(Message::from_subagent(result.id(), result.text()));
        if self.is_running() {
            AgentAvailability::InFlight
        } else {
            AgentAvailability::Available
        }
    }
}

/// One session's stable slot collection. A live graph node has exactly one slot.
pub(super) struct AgentSlots<Http: ClawHttp, Timer: ClawTimer> {
    slots: BTreeMap<AgentId, AgentSlot<Http, Timer>>,
}

impl<Http: ClawHttp, Timer: ClawTimer> AgentSlots<Http, Timer> {
    pub(super) fn new() -> Self {
        Self {
            slots: BTreeMap::new(),
        }
    }

    pub(super) fn insert(&mut self, id: AgentId, agent: BaseAgent<Http, Timer>) {
        match self.slots.entry(id) {
            Entry::Vacant(entry) => {
                entry.insert(AgentSlot::new(agent));
            }
            Entry::Occupied(_) => panic!("agent slot already exists: {id}"),
        }
    }

    pub(super) fn available_agent_mut(
        &mut self,
        id: AgentId,
    ) -> Option<&mut BaseAgent<Http, Timer>> {
        self.slots.get_mut(&id)?.idle_agent_mut()
    }

    pub(super) fn remove(&mut self, id: AgentId) -> bool {
        if let Some(slot) = self.slots.remove(&id) {
            slot.abort_if_running();
            true
        } else {
            false
        }
    }

    pub(super) fn take_idle(&mut self, id: AgentId) -> Option<BaseAgent<Http, Timer>> {
        self.slots.get_mut(&id)?.take_idle()
    }

    pub(super) fn start(
        &mut self,
        id: AgentId,
        is_root: bool,
        abort: AgentAbortHandle,
        future: AgentTickFuture<Http, Timer>,
    ) {
        self.slots
            .get_mut(&id)
            .unwrap_or_else(|| panic!("agent slot is missing: {id}"))
            .start(id, is_root, abort, future);
    }

    pub(super) fn activate_next_message(&mut self, id: AgentId) -> bool {
        self.slots
            .get_mut(&id)
            .is_some_and(AgentSlot::activate_next_message)
    }

    pub(super) fn deliver_child_result(
        &mut self,
        parent: AgentId,
        result: SubagentResult,
    ) -> Option<AgentAvailability> {
        Some(self.slots.get_mut(&parent)?.deliver_child_result(result))
    }

    pub(super) fn ready_inbox_ids(&self) -> impl Iterator<Item = AgentId> + '_ {
        self.slots.iter().filter_map(|(&id, slot)| {
            (slot.idle_agent().is_some() && !slot.inbox.is_empty()).then_some(id)
        })
    }

    pub(super) fn has_inbox(&self, id: AgentId) -> bool {
        self.slots
            .get(&id)
            .is_some_and(|slot| !slot.inbox.is_empty())
    }

    pub(super) fn has_inbox_except(&self, excluded: AgentId) -> bool {
        self.slots
            .iter()
            .any(|(&id, slot)| id != excluded && !slot.inbox.is_empty())
    }

    pub(super) fn first_inbox_origin(&self, id: AgentId) -> Option<TurnOrigin> {
        self.slots.get(&id)?.inbox.front().map(Message::origin)
    }

    pub(super) fn clear_inboxes(&mut self) {
        for slot in self.slots.values_mut() {
            slot.inbox.clear();
        }
    }

    pub(super) fn restore_inbox(&mut self, id: AgentId, inbox: Vec<Message>) {
        let slot = self
            .slots
            .get_mut(&id)
            .unwrap_or_else(|| panic!("agent slot is missing: {id}"));
        assert!(slot.inbox.is_empty(), "agent slot inbox is not empty: {id}");
        slot.inbox = inbox.into();
    }

    pub(super) fn views(&self) -> impl Iterator<Item = AgentSlotView<'_, Http, Timer>> + '_ {
        self.slots.iter().map(|(&id, slot)| AgentSlotView {
            id,
            agent: slot.idle_agent(),
            inbox: &slot.inbox,
        })
    }

    pub(super) fn is_running(&self, id: AgentId) -> bool {
        self.slots.get(&id).is_some_and(AgentSlot::is_running)
    }

    pub(super) fn has_running(&self) -> bool {
        self.slots.values().any(AgentSlot::is_running)
    }

    pub(super) fn has_running_root(&self) -> bool {
        self.slots.values().any(AgentSlot::is_running_root)
    }

    pub(super) fn has_running_background(&self) -> bool {
        self.slots
            .values()
            .any(|slot| slot.is_running() && !slot.is_running_root())
    }

    pub(super) fn abort_handles(&self) -> Vec<AgentAbortHandle> {
        self.slots
            .values()
            .filter_map(AgentSlot::abort_handle)
            .collect()
    }

    pub(super) fn abort_all(&self) {
        for slot in self.slots.values() {
            slot.abort_if_running();
        }
    }

    /// Cooperatively abort one running agent so a queued graph effect can retask it.
    pub(in crate::multiagent) fn abort_if_running(&self, id: AgentId) -> bool {
        self.slots.get(&id).is_some_and(AgentSlot::abort_if_running)
    }

    pub(super) fn next_completed<'a>(
        &'a mut self,
        control: &'a DriveControl,
    ) -> CompletedAgentTicks<'a, Http, Timer> {
        CompletedAgentTicks {
            slots: self,
            control,
            multiagent: None,
        }
    }

    pub(super) fn next_completed_or_command<'a>(
        &'a mut self,
        control: &'a DriveControl,
        multiagent: &'a MultiagentBridge,
    ) -> CompletedAgentTicks<'a, Http, Timer> {
        CompletedAgentTicks {
            slots: self,
            control,
            multiagent: Some(multiagent),
        }
    }
}

pub(super) struct CompletedAgentTicks<'a, Http: ClawHttp, Timer: ClawTimer> {
    slots: &'a mut AgentSlots<Http, Timer>,
    control: &'a DriveControl,
    multiagent: Option<&'a MultiagentBridge>,
}

impl<Http: ClawHttp, Timer: ClawTimer> Future for CompletedAgentTicks<'_, Http, Timer> {
    type Output = Vec<CompletedAgentTick>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();
        this.control.set_waker(context.waker().clone());
        if this.control.has_signal() {
            return Poll::Ready(Vec::new());
        }
        if this
            .multiagent
            .is_some_and(|host| host.register_waiter(context.waker()))
        {
            return Poll::Ready(Vec::new());
        }

        let mut completed = Vec::new();
        let mut pending = false;
        for (&id, slot) in &mut this.slots.slots {
            let polled = match slot.execution.as_mut() {
                Some(AgentExecution::Running(running)) => {
                    match running.future.as_mut().poll(context) {
                        Poll::Ready(output) => Some(output),
                        Poll::Pending => {
                            pending = true;
                            None
                        }
                    }
                }
                Some(AgentExecution::Idle(_)) => None,
                None => panic!("agent slot left in a transition state: {id}"),
            };
            if let Some(TickedAgent { agent, outcome }) = polled {
                slot.execution = Some(AgentExecution::Idle(agent));
                completed.push(CompletedAgentTick { id, outcome });
            }
        }

        if !completed.is_empty() {
            Poll::Ready(completed)
        } else if this
            .multiagent
            .is_some_and(|host| host.register_waiter(context.waker()))
        {
            Poll::Ready(Vec::new())
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
) -> AgentTickFuture<Http, Timer>
where
    Http: ClawHttp + StreamingHttp + 'static,
    Timer: ClawTimer + 'static,
{
    Box::pin(async move {
        let ReadyAgent {
            id,
            is_root,
            mut agent,
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
        TickedAgent { agent, outcome }
    })
}
