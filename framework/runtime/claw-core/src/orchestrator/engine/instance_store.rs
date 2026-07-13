use std::rc::Rc;

use claw_interface::http::StreamingHttp;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};

use crate::session::SessionId;

use super::super::instance::{InstanceWork, OrchestratorInstance, OrchestratorInstanceState};
use super::{Engine, SessionRuntime};

/// Holds an instance out of its session runtime and reinserts it on drop.
pub(super) struct InstanceSlot<'a, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    engine: &'a Engine<Filesystem, Http, Timer>,
    session_id: SessionId,
    instance: Option<OrchestratorInstance<Filesystem, Http, Timer>>,
}

impl<'a, Filesystem, Http, Timer> InstanceSlot<'a, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn new(
        engine: &'a Engine<Filesystem, Http, Timer>,
        session_id: SessionId,
        instance: OrchestratorInstance<Filesystem, Http, Timer>,
    ) -> Self {
        Self {
            engine,
            session_id,
            instance: Some(instance),
        }
    }

    pub(super) fn get_mut(&mut self) -> &mut OrchestratorInstance<Filesystem, Http, Timer> {
        self.instance
            .as_mut()
            .expect("InstanceSlot holds its instance until Drop")
    }
}

impl<Filesystem, Http, Timer> Drop for InstanceSlot<'_, Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn drop(&mut self) {
        if let Some(instance) = self.instance.take() {
            self.engine.put_instance(self.session_id, instance);
        }
    }
}

impl<Filesystem, Http, Timer> Engine<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    pub(super) fn instance_work(&self, session_id: SessionId) -> InstanceWork {
        self.runtimes
            .borrow()
            .get(&session_id)
            .map_or(InstanceWork::None, SessionRuntime::work)
    }

    pub(super) fn instance_has_active_approval(&self, session_id: SessionId) -> bool {
        self.runtimes
            .borrow()
            .get(&session_id)
            .is_some_and(SessionRuntime::has_active_approval)
    }

    pub(super) fn checkout_instance(
        &self,
        session_id: SessionId,
    ) -> InstanceSlot<'_, Filesystem, Http, Timer> {
        let instance = match self
            .runtimes
            .borrow_mut()
            .get_mut(&session_id)
            .expect("an instance is only checked out for a live session runtime")
            .take_instance()
        {
            Some(instance) => instance,
            None => OrchestratorInstance::new(
                session_id,
                Rc::clone(&self.factory),
                self.state.get().agent_id_allocator.clone(),
                OrchestratorInstanceState::default(),
            ),
        };
        InstanceSlot::new(self, session_id, instance)
    }

    pub(super) fn checkout_existing_instance(
        &self,
        session_id: SessionId,
    ) -> Option<InstanceSlot<'_, Filesystem, Http, Timer>> {
        let instance = self
            .runtimes
            .borrow_mut()
            .get_mut(&session_id)?
            .take_instance()?;
        Some(InstanceSlot::new(self, session_id, instance))
    }

    pub(super) fn put_instance(
        &self,
        session_id: SessionId,
        instance: OrchestratorInstance<Filesystem, Http, Timer>,
    ) {
        if !self.sessions.contains(session_id) {
            return;
        }
        if let Some(runtime) = self.runtimes.borrow_mut().get_mut(&session_id) {
            runtime.put_instance(instance);
        }
    }
}
