use std::collections::VecDeque;
use std::sync::{Arc, Mutex, MutexGuard};
use std::task::Waker;

use async_channel::{Receiver, Sender};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::protocol::{AgentId, AgentKind, Message};

use super::model::{
    MultiagentSnapshot, SubagentResult, SubagentSnapshot, SubagentSpec, SubagentTimeout,
};
use super::{AgentIdAllocator, MultiagentRuntime};

/// One command emitted by an agent's subagent tools.
pub(super) struct MultiagentCommand {
    requester: AgentId,
    action: MultiagentAction,
}

impl MultiagentCommand {
    pub(super) fn new(requester: AgentId, action: MultiagentAction) -> Self {
        Self { requester, action }
    }

    pub(super) fn into_parts(self) -> (AgentId, MultiagentAction) {
        (self.requester, self.action)
    }
}

pub(super) enum MultiagentAction {
    Spawn(SpawnCommand),
    Delete { target: AgentId },
    Followup { target: AgentId, message: Message },
}

pub(super) struct SpawnCommand {
    id: AgentId,
    spec: SubagentSpec,
    completion: Option<Sender<SubagentResult>>,
}

impl SpawnCommand {
    pub(super) fn into_parts(self) -> (AgentId, SubagentSpec, Option<Sender<SubagentResult>>) {
        (self.id, self.spec, self.completion)
    }
}

#[derive(Default)]
struct MultiagentBridgeState {
    commands: VecDeque<MultiagentCommand>,
    waiter: Option<Waker>,
    snapshot: MultiagentSnapshot,
}

/// Shared boundary between model-facing subagent tools and one session runtime.
///
/// Tools can only invoke the semantic methods on this bridge. The runtime alone
/// drains commands and publishes the inspection snapshot.
pub(in crate::multiagent) struct MultiagentBridge {
    agent_id_allocator: AgentIdAllocator,
    state: Mutex<MultiagentBridgeState>,
}

/// Caller-bound capability handed to model-facing subagent tools.
///
/// Binding the caller here prevents tools from choosing an arbitrary requester
/// when they enqueue commands or inspect the graph.
pub(super) struct SubagentControl {
    caller: AgentId,
    bridge: Arc<MultiagentBridge>,
}

impl SubagentControl {
    pub(super) fn new(caller: AgentId, bridge: Arc<MultiagentBridge>) -> Self {
        Self { caller, bridge }
    }

    pub(super) fn spawn_background(
        &self,
        kind: AgentKind,
        name: Option<String>,
        goal: Message,
        timeout: SubagentTimeout,
    ) -> AgentId {
        self.bridge
            .spawn_background(self.caller, SubagentSpec::new(kind, name, goal, timeout))
    }

    pub(super) fn spawn_foreground(
        &self,
        kind: AgentKind,
        name: Option<String>,
        goal: Message,
        timeout: SubagentTimeout,
    ) -> (AgentId, Receiver<SubagentResult>) {
        self.bridge
            .spawn_foreground(self.caller, SubagentSpec::new(kind, name, goal, timeout))
    }

    pub(super) fn delete(&self, target: AgentId) {
        self.bridge.delete(self.caller, target);
    }

    pub(super) fn followup(&self, target: AgentId, message: Message) {
        self.bridge.followup(self.caller, target, message);
    }

    pub(super) fn list(&self) -> Vec<SubagentSnapshot> {
        self.bridge.list(self.caller)
    }

    pub(super) fn get(&self, target: AgentId) -> Option<SubagentSnapshot> {
        self.bridge.get(self.caller, target)
    }
}

impl MultiagentBridge {
    pub(in crate::multiagent) fn new(agent_id_allocator: AgentIdAllocator) -> Self {
        Self {
            agent_id_allocator,
            state: Mutex::new(MultiagentBridgeState::default()),
        }
    }

    fn state(&self) -> MutexGuard<'_, MultiagentBridgeState> {
        self.state
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
    }

    fn push(&self, command: MultiagentCommand) {
        let waiter = {
            let mut state = self.state();
            state.commands.push_back(command);
            state.waiter.take()
        };
        if let Some(waiter) = waiter {
            waiter.wake();
        }
    }

    fn spawn(
        &self,
        parent: AgentId,
        spec: SubagentSpec,
        completion: Option<Sender<SubagentResult>>,
    ) -> AgentId {
        let id = self.agent_id_allocator.next();
        self.push(MultiagentCommand::new(
            parent,
            MultiagentAction::Spawn(SpawnCommand {
                id,
                spec,
                completion,
            }),
        ));
        id
    }

    pub(super) fn drain(&self) -> Vec<MultiagentCommand> {
        self.state().commands.drain(..).collect()
    }

    pub(super) fn requeue(&self, command: MultiagentCommand) {
        self.push(command);
    }

    pub(in crate::multiagent) fn clear(&self) {
        let mut state = self.state();
        state.commands.clear();
        state.waiter = None;
    }

    /// Register the drive waiting for an in-flight tick. Returns `true` when a
    /// command is already queued and the caller should apply it immediately.
    pub(in crate::multiagent) fn register_waiter(&self, waiter: &Waker) -> bool {
        let mut state = self.state();
        if !state.commands.is_empty() {
            return true;
        }
        state.waiter = Some(waiter.clone());
        false
    }

    pub(super) fn publish_snapshot(&self, snapshot: MultiagentSnapshot) {
        let mut state = self.state();
        state.snapshot = snapshot;
    }
}

impl MultiagentBridge {
    pub(super) fn spawn_background(&self, parent: AgentId, spec: SubagentSpec) -> AgentId {
        self.spawn(parent, spec, None)
    }

    pub(super) fn spawn_foreground(
        &self,
        parent: AgentId,
        spec: SubagentSpec,
    ) -> (AgentId, Receiver<SubagentResult>) {
        let (completion, result) = async_channel::bounded(1);
        let child = self.spawn(parent, spec, Some(completion));
        (child, result)
    }

    pub(super) fn delete(&self, requester: AgentId, target: AgentId) {
        self.push(MultiagentCommand::new(
            requester,
            MultiagentAction::Delete { target },
        ));
    }

    pub(super) fn followup(&self, requester: AgentId, target: AgentId, message: Message) {
        self.push(MultiagentCommand::new(
            requester,
            MultiagentAction::Followup { target, message },
        ));
    }

    pub(super) fn list(&self, requester: AgentId) -> Vec<SubagentSnapshot> {
        self.state().snapshot.descendants_of(requester)
    }

    pub(super) fn get(&self, requester: AgentId, target: AgentId) -> Option<SubagentSnapshot> {
        self.state().snapshot.descendant(requester, target)
    }
}

impl<Filesystem, Http, Timer> MultiagentRuntime<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::multiagent) fn refresh_multiagent_snapshot(&self) {
        let live = self
            .state
            .get()
            .nodes()
            .map(|(id, meta)| {
                SubagentSnapshot::new(
                    id,
                    meta.kind().clone(),
                    meta.name().map(str::to_owned),
                    meta.parent(),
                    self.state
                        .get()
                        .depth(id)
                        .expect("live graph topology is valid"),
                    self.state.get().agent_status(id, self.slots.is_running(id)),
                )
            })
            .collect::<Vec<_>>();
        let snapshot =
            MultiagentSnapshot::new(live.into_iter().chain(self.pending_deliveries.snapshots()));
        self.multiagent.publish_snapshot(snapshot);
    }
}
