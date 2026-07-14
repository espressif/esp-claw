use std::borrow::Cow;

use claw_checkpoint::{DurablePartError, DurableStateCodec, PartStateBlob, PartStateSlice};
use claw_permission::PermissionLevel;
use serde::{Deserialize, Serialize};

use crate::config::ReasoningEffort;
use crate::protocol::{Message, TurnId, TurnIdAllocator, TurnOrigin};

pub(crate) const SESSION_STATE_SCHEMA_VERSION: u32 = 5;

#[derive(Deserialize, Serialize)]
struct TurnState {
    id: TurnId,
    origin: TurnOrigin,
    pending_input: Option<Message>,
}

#[derive(Deserialize, Serialize)]
pub(crate) struct SessionState {
    active_turn: Option<TurnState>,
    next_turn_id: TurnId,
    reasoning_effort: ReasoningEffort,
    pending_reasoning_effort: Option<ReasoningEffort>,
    permission_level: PermissionLevel,
}

impl Default for SessionState {
    fn default() -> Self {
        Self {
            active_turn: None,
            next_turn_id: TurnIdAllocator::new().peek(),
            reasoning_effort: ReasoningEffort::default(),
            pending_reasoning_effort: None,
            permission_level: PermissionLevel::default(),
        }
    }
}

impl SessionState {
    pub(super) fn has_active_turn(&self) -> bool {
        self.active_turn.is_some()
    }

    pub(super) fn active_turn_id(&self) -> Option<TurnId> {
        self.active_turn.as_ref().map(|turn| turn.id)
    }

    pub(super) fn active_turn_origin(&self) -> Option<TurnOrigin> {
        self.active_turn.as_ref().map(|turn| turn.origin)
    }

    pub(super) fn has_pending_input(&self) -> bool {
        self.active_turn
            .as_ref()
            .is_some_and(|turn| turn.pending_input.is_some())
    }

    pub(super) fn begin_user_turn(&mut self, input: Message) -> TurnId {
        self.begin_turn(TurnOrigin::User, Some(input.into_user()))
    }

    pub(super) fn begin_subagent_turn(&mut self, origin: TurnOrigin) -> TurnId {
        debug_assert!(matches!(origin, TurnOrigin::Subagent { .. }));
        self.begin_turn(origin, None)
    }

    fn begin_turn(&mut self, origin: TurnOrigin, pending_input: Option<Message>) -> TurnId {
        debug_assert!(self.active_turn.is_none());
        if let Some(effort) = self.pending_reasoning_effort.take() {
            self.reasoning_effort = effort;
        }
        let id = self.next_turn_id;
        self.next_turn_id = TurnId::new(id.0.saturating_add(1));
        self.active_turn = Some(TurnState {
            id,
            origin,
            pending_input,
        });
        id
    }

    pub(super) fn continue_turn(&mut self, input: Message) {
        if let Some(turn) = self.active_turn.as_mut() {
            turn.pending_input = Some(input);
        }
    }

    pub(super) fn take_pending_input(&mut self) -> Option<Message> {
        self.active_turn.as_mut()?.pending_input.take()
    }

    pub(super) fn finish_turn(&mut self) -> Option<TurnId> {
        Some(self.active_turn.take()?.id)
    }

    pub(super) fn set_reasoning_effort(&mut self, effort: ReasoningEffort) {
        self.pending_reasoning_effort = Some(effort);
    }

    pub(super) fn reasoning_effort(&self) -> ReasoningEffort {
        self.reasoning_effort
    }

    pub(super) fn set_permission_level(&mut self, level: PermissionLevel) {
        self.permission_level = level;
    }

    pub(super) fn permission_level(&self) -> PermissionLevel {
        self.permission_level
    }
}

impl DurableStateCodec for SessionState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: SESSION_STATE_SCHEMA_VERSION,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        if state.schema_version != SESSION_STATE_SCHEMA_VERSION {
            return Err(DurablePartError::InvalidState(
                "unsupported session-state checkpoint schema",
            ));
        }
        serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)
    }
}

#[cfg(test)]
mod tests {
    use claw_checkpoint::DurableStateCodec;

    use super::{SessionState, SESSION_STATE_SCHEMA_VERSION};
    use claw_permission::PermissionLevel;

    use crate::protocol::{Message, TurnOrigin};

    #[test]
    fn active_turn_round_trips_through_the_current_schema() {
        let mut state = SessionState::default();
        state.set_permission_level(PermissionLevel::Ask);
        let turn = state.begin_user_turn(Message::text("hello"));
        let encoded = state.encode_state().unwrap().into_owned();
        assert_eq!(encoded.schema_version, SESSION_STATE_SCHEMA_VERSION);

        let restored = SessionState::decode_state(encoded.as_slice()).unwrap();
        assert_eq!(restored.active_turn_id(), Some(turn));
        assert_eq!(restored.active_turn_origin(), Some(TurnOrigin::User));
        assert!(restored.has_pending_input());
        assert_eq!(restored.permission_level(), PermissionLevel::Ask);
    }
}
