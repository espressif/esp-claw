use core::pin::Pin;
use core::task::{Context, Poll};

use async_channel::{Receiver, Sender};
use futures_core::Stream;
use strum::IntoStaticStr;

use crate::config::ReasoningEffort;
use crate::protocol::{EventSink, Message, SessionEvent, SessionId};

/// Failure opening a session event stream.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum OpenSessionError {
    #[error("session not found: {0}")]
    SessionNotFound(SessionId),
    #[error("session is already open: {0}")]
    AlreadyOpen(SessionId),
    #[error("orchestrator worker is not running")]
    WorkerStopped,
}

/// Failure sending a command through a session control handle.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum SessionControlError {
    #[error("session is closed: {0}")]
    SessionClosed(SessionId),
    #[error("session is busy: {0}")]
    Busy(SessionId),
    #[error("orchestrator worker is not running")]
    WorkerStopped,
    #[error("failed to persist closed session state")]
    ClosePersistence,
}

#[derive(Clone, Copy, Debug, IntoStaticStr, PartialEq, Eq)]
pub(crate) enum ControlOp {
    #[strum(serialize = "interrupt")]
    Interrupt,
    #[strum(serialize = "cancel")]
    Cancel,
}

impl ControlOp {
    pub(crate) fn merge(existing: Option<Self>, incoming: Self) -> Self {
        match (existing, incoming) {
            (Some(Self::Cancel), _) | (_, Self::Cancel) => Self::Cancel,
            _ => Self::Interrupt,
        }
    }
}

pub(crate) struct SessionEndpoint {
    lease: u64,
    commands: Sender<SessionCommand>,
}

impl SessionEndpoint {
    pub(crate) fn new(lease: u64, commands: Sender<SessionCommand>) -> Self {
        Self { lease, commands }
    }
}

/// Commands addressed directly to one live session actor.
pub(crate) enum SessionCommand {
    Open {
        events: EventSink,
        commands: Sender<SessionCommand>,
        ack: Sender<Result<SessionEndpoint, OpenSessionError>>,
    },
    Submit {
        lease: u64,
        message: Message,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Control {
        lease: u64,
        op: ControlOp,
        ack: Sender<Result<(), SessionControlError>>,
    },
    SetReasoningEffort {
        lease: u64,
        effort: ReasoningEffort,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Close {
        lease: u64,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Delete {
        ack: Sender<Result<(), SessionControlError>>,
    },
    Shutdown,
}

/// Cloneable write/control half of an open session.
#[derive(Clone)]
pub struct SessionControl {
    lease: u64,
    commands: Sender<SessionCommand>,
}

impl SessionControl {
    pub(crate) fn new(endpoint: SessionEndpoint) -> Self {
        Self {
            lease: endpoint.lease,
            commands: endpoint.commands,
        }
    }

    /// Submit one message for this session.
    ///
    /// This resolves when the actor accepts the message, not when the turn ends.
    pub async fn submit(&self, message: Message) -> Result<(), SessionControlError> {
        let (ack, result) = async_channel::bounded(1);
        self.commands
            .send(SessionCommand::Submit {
                lease: self.lease,
                message,
                ack,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        result
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
    }

    /// Apply a new reasoning effort at the next turn boundary.
    pub async fn set_reasoning_effort(
        &self,
        effort: ReasoningEffort,
    ) -> Result<(), SessionControlError> {
        let (ack, result) = async_channel::bounded(1);
        self.commands
            .send(SessionCommand::SetReasoningEffort {
                lease: self.lease,
                effort,
                ack,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        result
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
    }

    /// Stop the current foreground turn while preserving live subagents.
    pub async fn interrupt(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Interrupt).await
    }

    /// Cancel the current turn and all background subagents in this session.
    pub async fn cancel(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Cancel).await
    }

    /// Close this event stream and checkpoint the session. The session id stays live.
    pub async fn close_session(&self) -> Result<(), SessionControlError> {
        let (ack, result) = async_channel::bounded(1);
        self.commands
            .send(SessionCommand::Close {
                lease: self.lease,
                ack,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        result
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
    }

    async fn send_control(&self, op: ControlOp) -> Result<(), SessionControlError> {
        let (ack, result) = async_channel::bounded(1);
        self.commands
            .send(SessionCommand::Control {
                lease: self.lease,
                op,
                ack,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        result
            .recv()
            .await
            .unwrap_or(Err(SessionControlError::WorkerStopped))
    }
}

/// The read/event half of an open session.
pub struct SessionEventStream {
    events: Pin<Box<Receiver<SessionEvent>>>,
}

impl SessionEventStream {
    pub(crate) fn new(events: Receiver<SessionEvent>) -> Self {
        Self {
            events: Box::pin(events),
        }
    }
}

impl Stream for SessionEventStream {
    type Item = SessionEvent;

    fn poll_next(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Option<Self::Item>> {
        self.get_mut().events.as_mut().poll_next(context)
    }
}
