//! In-memory ingress queues for the orchestrator.

use std::collections::VecDeque;
use std::sync::Mutex;

use super::{AgentEvent, Command};

#[derive(Default)]
pub struct CommandQueue {
    inner: Mutex<VecDeque<Command>>,
}

impl CommandQueue {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&self, command: Command) {
        self.inner.lock().unwrap().push_back(command);
    }

    pub fn drain(&self) -> Vec<Command> {
        let mut guard = self.inner.lock().unwrap();
        guard.drain(..).collect()
    }
}

#[derive(Default)]
pub struct AgentEventQueue {
    inner: Mutex<VecDeque<AgentEvent>>,
}

impl AgentEventQueue {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn push(&self, event: AgentEvent) {
        self.inner.lock().unwrap().push_back(event);
    }

    pub fn drain(&self) -> Vec<AgentEvent> {
        let mut guard = self.inner.lock().unwrap();
        guard.drain(..).collect()
    }
}
