use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::VecDeque;
use std::rc::Rc;

use async_channel::Receiver;
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use futures_core::Stream;

use crate::orchestrator::{OpenSessionError, SessionControlError};

use super::super::session_drive::SubmittedInput;
use super::super::session_io::Command;
use super::super::{DriveFuture, Engine};

enum EngineEvent {
    DriveDone,
    Command(Option<Command>),
}

struct EnginePoll<'a> {
    inflight: &'a mut VecDeque<DriveFuture>,
    recv: Option<Pin<&'a mut Receiver<Command>>>,
}

impl Future for EnginePoll<'_> {
    type Output = EngineEvent;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        if let Some(receiver) = self.recv.as_mut() {
            if let Poll::Ready(command) = receiver.as_mut().poll_next(context) {
                return Poll::Ready(EngineEvent::Command(command));
            }
        }

        let count = self.inflight.len();
        for _ in 0..count {
            let Some(mut future) = self.inflight.pop_front() else {
                break;
            };
            if future.as_mut().poll(context).is_ready() {
                return Poll::Ready(EngineEvent::DriveDone);
            }
            self.inflight.push_back(future);
        }

        Poll::Pending
    }
}

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(in crate::orchestrator::engine) async fn run(
        self: &Rc<Self>,
        command_rx: Receiver<Command>,
    ) {
        let mut command_rx = core::pin::pin!(command_rx);
        let mut inflight: VecDeque<DriveFuture> = VecDeque::new();
        let mut rx_open = true;
        let mut stopping = false;

        loop {
            if (stopping || !rx_open) && inflight.is_empty() {
                self.reject_queued_commands_after_stop(&command_rx, stopping);
                return;
            }
            let recv = (rx_open && !stopping).then_some(command_rx.as_mut());
            match (EnginePoll {
                inflight: &mut inflight,
                recv,
            })
            .await
            {
                EngineEvent::DriveDone => {}
                EngineEvent::Command(Some(Command::OpenSession {
                    session,
                    events,
                    ack,
                })) => {
                    let (result, future) = self.open_session_stream(session, events);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::Submit { session, text, ack })) => {
                    let (result, future) = self.submit_input(session, SubmittedInput { text });
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::Control { session, op, ack })) => {
                    let (result, future) = self.control_session(session, op);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::SetReasoningEffort {
                    session,
                    effort,
                    ack,
                })) => {
                    let _ = ack.try_send(self.set_reasoning_effort(session, effort));
                }
                EngineEvent::Command(Some(Command::CloseSession { session, ack })) => {
                    let (result, future) = self.close_session(session);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::DeleteSession { session, ack })) => {
                    let (result, future) = self.delete_session(session);
                    let _ = ack.try_send(result);
                    if let Some(future) = future {
                        inflight.push_back(future);
                    }
                }
                EngineEvent::Command(Some(Command::Stop)) => stopping = true,
                EngineEvent::Command(None) => rx_open = false,
            }
        }
    }

    fn reject_queued_commands_after_stop(&self, command_rx: &Receiver<Command>, stopping: bool) {
        if !stopping {
            return;
        }
        while let Ok(command) = command_rx.try_recv() {
            match command {
                Command::OpenSession { ack, .. } => {
                    let _ = ack.try_send(Err(OpenSessionError::WorkerStopped));
                }
                Command::Submit { ack, .. }
                | Command::Control { ack, .. }
                | Command::SetReasoningEffort { ack, .. }
                | Command::CloseSession { ack, .. }
                | Command::DeleteSession { ack, .. } => {
                    let _ = ack.try_send(Err(SessionControlError::WorkerStopped));
                }
                Command::Stop => {}
            }
        }
    }
}
