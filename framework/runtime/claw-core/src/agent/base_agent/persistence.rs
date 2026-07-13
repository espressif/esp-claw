use std::borrow::Cow;

use claw_checkpoint::{
    ChangePatternHint, DurablePart, DurablePartError, DurableState, DurableStateCodec,
    PartGeneration, PartStateBlob, PartStateSlice, StorageHint, StorageSizeHint,
};
use claw_interface::{ClawHttp, ClawTimer};

use super::state::BaseAgentState;
use super::BaseAgent;

const BASE_AGENT_SCHEMA_VERSION: u32 = 2;

impl DurableStateCodec for BaseAgentState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        let bytes = serde_json::to_vec(self).map_err(DurablePartError::Encode)?;
        Ok(PartStateBlob {
            schema_version: BASE_AGENT_SCHEMA_VERSION,
            bytes: Cow::Owned(bytes),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        if state.schema_version != BASE_AGENT_SCHEMA_VERSION {
            return Err(DurablePartError::InvalidState(
                "unsupported base-agent schema version",
            ));
        }
        let decoded: Self =
            serde_json::from_slice(state.bytes).map_err(DurablePartError::Decode)?;
        decoded
            .task()
            .validate()
            .map_err(|_| DurablePartError::InvalidState("invalid task mailbox"))?;
        Ok(decoded)
    }
}

impl<H: ClawHttp, Timer: ClawTimer> DurablePart for BaseAgent<H, Timer> {
    fn name(&self) -> &'static str {
        "base-agent"
    }

    fn generation(&self) -> PartGeneration {
        self.state.generation()
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        self.state.export_state()
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Large,
            change: ChangePatternHint::Arbitrary,
        }
    }
}

impl<H: ClawHttp, Timer: ClawTimer> BaseAgent<H, Timer> {
    fn restore_state(&mut self, state: PartStateSlice<'_>) -> Result<(), DurablePartError> {
        self.state = DurableState::restore_state(state)?;
        self.outcome = None;
        self.interruption.clear();
        Ok(())
    }

    pub(crate) fn durable_parts(&self) -> Vec<&dyn DurablePart> {
        vec![self, &self.tools]
    }

    pub(crate) fn restore_durable_part(
        &mut self,
        name: &str,
        state: PartStateSlice<'_>,
    ) -> Result<bool, DurablePartError> {
        match name {
            "base-agent" => {
                self.restore_state(state)?;
                Ok(true)
            }
            "tool-set" => {
                self.tools.restore_state(state)?;
                Ok(true)
            }
            _ => Ok(false),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::agent::base_agent::task_state::TaskAction;
    use crate::agent::base_agent::{AgentCommand, ApprovalDecision};

    #[test]
    fn schema_two_round_trips_the_canonical_approval_payload() {
        let mut state = BaseAgentState::new(0);
        state.task_mut().enqueue_task_input("start".into());
        let _ = state.task_mut().pop_action().expect("valid task input");
        state
            .task_mut()
            .await_approval(vec!["signature-a".into(), "signature-b".into()])
            .expect("running task can await approval");

        let encoded = state.encode_state().expect("state encodes").into_owned();
        assert_eq!(encoded.schema_version, BASE_AGENT_SCHEMA_VERSION);

        let mut restored =
            BaseAgentState::decode_state(encoded.as_slice()).expect("schema two state round trips");
        restored
            .task_mut()
            .enqueue_command(AgentCommand::ApprovalResult(ApprovalDecision::Approved))
            .expect("restored approval accepts its matching decision");
        assert!(matches!(
            restored
                .task_mut()
                .pop_action()
                .expect("valid restored queue"),
            Some(TaskAction::ApprovalResult {
                grant_signatures,
                ..
            }) if grant_signatures == ["signature-a", "signature-b"]
        ));
    }

    #[test]
    fn unsupported_schema_is_rejected_explicitly() {
        let result = BaseAgentState::decode_state(PartStateSlice {
            schema_version: BASE_AGENT_SCHEMA_VERSION + 1,
            bytes: b"{}",
        });

        assert!(matches!(result, Err(DurablePartError::InvalidState(_))));
    }
}
