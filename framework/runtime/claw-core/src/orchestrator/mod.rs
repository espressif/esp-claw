//! Layer 1 orchestrator: session registry and per-session agent graph driving.
//!
//! Channel routing is owned by the layer above this crate.

mod control;
mod instance;

use std::collections::{HashMap, VecDeque};
use std::sync::{Arc, Mutex};

use claw_api::ClawApiConfig;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_utils::{async_oneshot, AsyncOneshotSender};

use crate::agent::{
    AgentIdAllocator, AgentResolver, CancelReason, FsAgentFactory, FsAgentFactoryError,
};
use crate::session::{
    DeliverError, DeliveryKind, SessionError, SessionId, SessionMessage, SessionRecord,
    SessionStore,
};

pub use self::instance::{ApprovalRequest, DriveOutput, RootReply};

use self::control::{DriveStop, SessionControl};
use self::instance::OrchestratorInstance;

type SubmitReply = AsyncOneshotSender<Result<DriveOutput, DeliverError>>;

struct QueuedSubmission {
    msg: SessionMessage,
    reply: SubmitReply,
}

#[derive(Default)]
struct SessionDrive {
    active: bool,
    control: Option<SessionControl>,
    carried: Option<QueuedSubmission>,
    pending: VecDeque<QueuedSubmission>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum StartMode {
    Fresh,
    Interrupted,
    Cancelled,
}

/// RAII checkout of a session's [`OrchestratorInstance`]: holds the instance out
/// of the map while it is driven and reinserts it on drop, so no exit path (an
/// early `?`, a panic while driving, or normal return) can drop the graph.
struct InstanceSlot<'a, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    orchestrator: &'a Orchestrator<F, H, Timer>,
    session_id: SessionId,
    instance: Option<OrchestratorInstance<F, H, Timer>>,
}

impl<F, H, Timer> InstanceSlot<'_, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn get_mut(&mut self) -> &mut OrchestratorInstance<F, H, Timer> {
        // Invariant: `instance` is `Some` for the whole lifetime of the slot;
        // it is only taken in `Drop`, after which the slot is unreachable.
        self.instance
            .as_mut()
            .expect("InstanceSlot holds its instance until Drop")
    }
}

impl<F, H, Timer> Drop for InstanceSlot<'_, F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn drop(&mut self) {
        if let Some(instance) = self.instance.take() {
            self.orchestrator.put_instance(self.session_id, instance);
        }
    }
}

pub struct Orchestrator<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Builds agents for every session's registry. Required at construction
    /// time: the orchestrator owns no LLM client of its own — the factory holds
    /// whatever an agent needs to run.
    factory: Arc<FsAgentFactory<F, H, Timer>>,
    /// Global agent-id allocator shared by every per-session registry so ids are
    /// unique across the whole process, not merely within one session.
    next_agent_id: AgentIdAllocator,
    /// One isolated agent graph per session. The map lock is held only while an
    /// instance is inserted, removed, or taken for driving; it is not held while
    /// the agent graph awaits LLM/tool work.
    instances: Mutex<HashMap<SessionId, OrchestratorInstance<F, H, Timer>>>,
    /// Per-session delivery state. This is the owner of append/interrupt/cancel
    /// sequencing while an instance is checked out and awaiting LLM/tool work.
    drives: Mutex<HashMap<SessionId, SessionDrive>>,
    sessions: SessionStore,
}

impl<F, H, Timer> Orchestrator<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build an orchestrator and its concrete filesystem-backed agent factory.
    ///
    /// `resolver` maps manifest-declared capability names to handlers,
    /// `llm_config` is cloned into every agent, and `persistence_dir` is the
    /// storage root the factory owns below this orchestrator.
    ///
    /// # Errors
    ///
    /// Returns [`FsAgentFactoryError`] when the factory cannot be assembled.
    pub fn new(
        resolver: Arc<dyn AgentResolver>,
        llm_config: ClawApiConfig,
        persistence_dir: &str,
    ) -> Result<Self, FsAgentFactoryError> {
        let factory = Arc::new(FsAgentFactory::<F, H, Timer>::new(
            resolver,
            llm_config,
            persistence_dir,
        )?);
        Ok(Self {
            factory,
            next_agent_id: AgentIdAllocator::new(),
            instances: Mutex::new(HashMap::new()),
            drives: Mutex::new(HashMap::new()),
            sessions: SessionStore::new(),
        })
    }

    /// Submit one message to a live session.
    ///
    /// This is the only public delivery entry point. It owns the per-session
    /// sequencing for [`DeliveryKind::Append`], [`DeliveryKind::Interrupt`], and
    /// [`DeliveryKind::Cancel`]: append waits for the active drive to settle,
    /// interrupt asks the active drive to stop after its current batch, and cancel
    /// aborts the active batch through the session's cancel hook.
    pub async fn submit(
        &self,
        session_id: SessionId,
        msg: SessionMessage,
    ) -> Result<DriveOutput, DeliverError> {
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }

        let (reply, receiver) = async_oneshot();
        let should_drive = self.enqueue_submission(session_id, QueuedSubmission { msg, reply });

        if should_drive {
            self.drive_submissions(session_id).await;
        }

        receiver.await.unwrap_or_else(|| {
            Err(DeliverError::Agent(
                "session submit driver ended before replying".to_string(),
            ))
        })
    }

    fn enqueue_submission(&self, session_id: SessionId, submission: QueuedSubmission) -> bool {
        let mut drives = self
            .drives
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let drive = drives.entry(session_id).or_default();
        if drive.active {
            match submission.msg.kind {
                DeliveryKind::Append => drive.pending.push_back(submission),
                DeliveryKind::Interrupt if drive.carried.is_none() => {
                    if let Some(control) = &drive.control {
                        control.request_interrupt();
                    }
                    drive.carried = Some(submission);
                }
                DeliveryKind::Interrupt => {
                    drive.pending.push_back(submission);
                }
                DeliveryKind::Cancel => {
                    if let Some(control) = &drive.control {
                        control.request_cancel();
                    }
                    if let Some(replaced) = drive.carried.replace(submission) {
                        let _ = replaced.reply.send(Err(DeliverError::Superseded));
                    }
                }
            }
            false
        } else {
            drive.active = true;
            drive.pending.push_back(submission);
            true
        }
    }

    async fn drive_submissions(&self, session_id: SessionId) {
        let mut mode = StartMode::Fresh;
        loop {
            let Some(submission) = self.take_next_submission(session_id) else {
                self.finish_drive(session_id);
                return;
            };
            let (result, stop) = match self
                .drive_one_submission(session_id, submission.msg.clone(), mode)
                .await
            {
                Ok((output, stop)) => (Ok(output), stop),
                Err(error) => (Err(error), DriveStop::Quiescent),
            };
            let _ = submission.reply.send(result);
            mode = match stop {
                DriveStop::Quiescent => StartMode::Fresh,
                DriveStop::Interrupted => StartMode::Interrupted,
                DriveStop::Cancelled => StartMode::Cancelled,
            };
        }
    }

    fn take_next_submission(&self, session_id: SessionId) -> Option<QueuedSubmission> {
        let mut drives = self
            .drives
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        let drive = drives.get_mut(&session_id)?;
        if let Some(submission) = drive.carried.take() {
            return Some(submission);
        }
        drive.pending.pop_front()
    }

    fn finish_drive(&self, session_id: SessionId) {
        let mut drives = self
            .drives
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        if let Some(drive) = drives.get_mut(&session_id) {
            drive.active = false;
            drive.control = None;
        }
    }

    fn set_active_control(&self, session_id: SessionId, control: Option<SessionControl>) {
        let mut drives = self
            .drives
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        if let Some(drive) = drives.get_mut(&session_id) {
            if let Some(control) = &control {
                if let Some(submission) = &drive.carried {
                    match submission.msg.kind {
                        DeliveryKind::Interrupt => control.request_interrupt(),
                        DeliveryKind::Cancel => control.request_cancel(),
                        DeliveryKind::Append => {}
                    }
                }
            }
            drive.control = control;
        }
    }

    async fn drive_one_submission(
        &self,
        session_id: SessionId,
        msg: SessionMessage,
        mode: StartMode,
    ) -> Result<(DriveOutput, DriveStop), DeliverError> {
        if !self.sessions.contains(session_id) {
            return Err(DeliverError::SessionNotFound(session_id));
        }

        // The instance is checked out of the map so it can be driven without
        // holding the map lock across `.await`. `InstanceSlot` is an RAII guard:
        // it reinserts the (possibly mutated) instance on every exit path — the
        // `?` below, a `drive().await` panic, or normal return — so a session's
        // agent graph is never silently dropped.
        //
        // Delivery is assumed to be serialized per session by the driving layer
        // (one agent executor). Two concurrent `deliver`s for the same session
        // would each check out a slot and the last reinsert would win; the
        // channel router does not currently issue such concurrent calls.
        let mut slot = self.checkout_instance(session_id);
        let instance = slot.get_mut();
        let turn = instance.next_turn();
        // session > turn: the session span opens `conversation.session`, the
        // turn span opens `conversation.turn`. Every agent/iteration/tool span
        // produced while driving nests under them, so one drive reads as a unit.
        let _session_span =
            tracing::info_span!("session", conversation.session = %session_id).entered();
        let _turn_span = tracing::info_span!(
            "turn",
            conversation.turn = turn,
            message_id = %msg.message_id,
            cause = "message"
        )
        .entered();

        match mode {
            StartMode::Fresh => {
                if msg.kind == DeliveryKind::Cancel {
                    instance.cancel_root(CancelReason::Superseded);
                }
                instance
                    .deliver(msg.text.clone())
                    .map_err(DeliverError::Agent)?;
            }
            StartMode::Interrupted => {
                instance
                    .interrupt_root(msg.text.clone())
                    .map_err(DeliverError::Agent)?;
            }
            StartMode::Cancelled => {
                instance.cancel_root(CancelReason::Superseded);
                instance
                    .deliver(msg.text.clone())
                    .map_err(DeliverError::Agent)?;
            }
        }

        let control = SessionControl::new();
        self.set_active_control(session_id, Some(control.clone()));
        let output = instance.drive_interruptible(&control).await;
        self.set_active_control(session_id, None);
        Ok(output)
    }

    /// Check the session's agent graph out of the map (building a fresh instance
    /// when the session has none yet), wrapped in an [`InstanceSlot`] that
    /// reinserts it on drop.
    fn checkout_instance(&self, session_id: SessionId) -> InstanceSlot<'_, F, H, Timer> {
        let instance = self
            .instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id)
            .unwrap_or_else(|| {
                OrchestratorInstance::new(
                    session_id,
                    Arc::clone(&self.factory),
                    self.next_agent_id.clone(),
                )
            });
        InstanceSlot {
            orchestrator: self,
            session_id,
            instance: Some(instance),
        }
    }

    fn put_instance(&self, session_id: SessionId, instance: OrchestratorInstance<F, H, Timer>) {
        if !self.sessions.contains(session_id) {
            return;
        }
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .insert(session_id, instance);
    }

    pub fn session_create(&self) -> SessionId {
        self.sessions.create().id
    }

    pub fn session_list(&self) -> Vec<SessionRecord> {
        let mut sessions = self.sessions.list();
        sessions.sort_by_key(|record| record.id.0);
        sessions
    }

    /// Persist `session_id`'s channel binding so it can be rebuilt after a
    /// restart. See [`SessionStore::set_binding`].
    ///
    /// # Errors
    ///
    /// [`SessionError::NotFound`] when `session_id` is not registered.
    pub fn session_set_binding(
        &self,
        session_id: SessionId,
        channel: impl Into<String>,
        chat_id: impl Into<String>,
    ) -> Result<(), SessionError> {
        self.sessions.set_binding(session_id, channel, chat_id)
    }

    pub fn session_delete(&self, session_id: SessionId) -> Result<(), SessionError> {
        self.sessions.delete(session_id)?;
        // Drop the session's agent graph so a deleted session leaves no live
        // agents behind.
        self.instances
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id);
        self.drives
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
            .remove(&session_id);
        Ok(())
    }
}
