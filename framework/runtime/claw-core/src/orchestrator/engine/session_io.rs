use core::pin::Pin;
use core::task::{Context, Poll};

use async_channel::{Receiver, Sender};
use futures_core::Stream;
use strum::IntoStaticStr;

use crate::event::{EventSink, SessionEvent};
use crate::orchestrator::{
    OpenSessionError, ReasoningEffort, SessionControl, SessionControlError, SessionEventStream,
};
use crate::session::{Message, SessionId};

use super::super::control::DriveControl;

#[derive(Clone, Copy, Debug, IntoStaticStr, PartialEq, Eq)]
pub(in crate::orchestrator) enum ControlOp {
    #[strum(serialize = "interrupt")]
    Interrupt,
    #[strum(serialize = "cancel")]
    Cancel,
}

impl ControlOp {
    pub(super) fn merge(existing: Option<Self>, incoming: Self) -> Self {
        match (existing, incoming) {
            (Some(Self::Cancel), _) | (_, Self::Cancel) => Self::Cancel,
            _ => Self::Interrupt,
        }
    }
}

/// A command the `Orchestrator` handle sends to its engine worker.
pub(in crate::orchestrator) enum Command {
    OpenSession {
        session: SessionId,
        events: EventSink,
        ack: Sender<Result<(), OpenSessionError>>,
    },
    Submit {
        session: SessionId,
        message: Message,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Control {
        session: SessionId,
        op: ControlOp,
        ack: Sender<Result<(), SessionControlError>>,
    },
    SetReasoningEffort {
        session: SessionId,
        effort: ReasoningEffort,
        ack: Sender<Result<(), SessionControlError>>,
    },
    CloseSession {
        session: SessionId,
        ack: Sender<Result<(), SessionControlError>>,
    },
    DeleteSession {
        session: SessionId,
        ack: Sender<Result<(), SessionControlError>>,
    },
    Stop,
}

impl SessionControl {
    pub(in crate::orchestrator) fn new(session: SessionId, command_tx: Sender<Command>) -> Self {
        Self {
            session,
            command_tx,
        }
    }

    /// Submit one text message for this session.
    ///
    /// The returned future resolves when the orchestrator accepts the command,
    /// not when the agent reply completes. If a foreground submit is already in
    /// progress, this returns [`SessionControlError::Busy`] instead of buffering
    /// another input internally. User-visible output is delivered on the paired
    /// [`SessionEventStream`].
    pub async fn submit(&self, message: Message) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::Submit {
                session: self.session,
                message,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }

    /// Reconfigure this session's reasoning effort.
    ///
    /// A config-only request: it does not disturb a turn already running. The new
    /// effort takes effect on the session's next turn. The returned future
    /// resolves when the orchestrator accepts the command.
    pub async fn set_reasoning_effort(
        &self,
        effort: ReasoningEffort,
    ) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::SetReasoningEffort {
                session: self.session,
                effort,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }

    /// Gracefully stop the current foreground drive at the next safe boundary.
    pub async fn interrupt(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Interrupt).await
    }

    /// Hard-cancel foreground and background work in this session.
    pub async fn cancel(&self) -> Result<(), SessionControlError> {
        self.send_control(ControlOp::Cancel).await
    }

    /// Close this open session stream and checkpoint its final runtime state.
    /// The session id remains live and can be opened again later; deleting the
    /// session is handled by the orchestrator handle.
    ///
    /// The stream is closed even when the final checkpoint fails. In that case,
    /// this returns [`SessionControlError::ClosePersistence`].
    pub async fn close_session(&self) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::CloseSession {
                session: self.session,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }

    async fn send_control(&self, op: ControlOp) -> Result<(), SessionControlError> {
        let (ack_tx, ack_rx) = async_channel::bounded(1);
        self.command_tx
            .send(Command::Control {
                session: self.session,
                op,
                ack: ack_tx,
            })
            .await
            .map_err(|_| SessionControlError::WorkerStopped)?;
        match ack_rx.recv().await {
            Ok(result) => result,
            Err(_) => Err(SessionControlError::WorkerStopped),
        }
    }
}

impl SessionEventStream {
    pub(in crate::orchestrator) fn new(events: Receiver<SessionEvent>) -> Self {
        Self {
            events: Box::pin(events),
        }
    }
}

impl Stream for SessionEventStream {
    type Item = SessionEvent;

    fn poll_next(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Option<SessionEvent>> {
        self.get_mut().events.as_mut().poll_next(context)
    }
}

pub(super) fn apply_control(control: &DriveControl, op: Option<ControlOp>) {
    match op {
        Some(ControlOp::Interrupt) => control.request_interrupt(),
        Some(ControlOp::Cancel) => control.request_cancel(),
        None => {}
    }
}
