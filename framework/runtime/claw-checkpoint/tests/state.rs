#![allow(clippy::unwrap_used)]

use std::borrow::Cow;

use claw_checkpoint::{
    DurablePartError, DurableState, DurableStateCodec, PartStateBlob, PartStateSlice,
};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
struct TestState {
    value: Vec<u8>,
}

impl DurableStateCodec for TestState {
    fn encode_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Borrowed(&self.value),
        })
    }

    fn decode_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError> {
        Ok(Self {
            value: state.bytes.to_vec(),
        })
    }
}

#[test]
fn mutation_bumps_generation() {
    let mut state = DurableState::new(TestState::default());
    assert_eq!(state.generation(), 0);

    state.get_mut().value.push(1);
    assert_eq!(state.generation(), 1);

    state.replace(TestState { value: vec![2] });
    assert_eq!(state.generation(), 2);
}

#[test]
fn exports_and_restores_codec_state() {
    let state = DurableState::new(TestState {
        value: vec![1, 2, 3],
    });
    let blob = state.export_state().unwrap();
    assert_eq!(blob.schema_version, 1);
    assert_eq!(blob.bytes.as_ref(), &[1, 2, 3]);

    let restored = DurableState::<TestState>::restore_state(PartStateSlice {
        schema_version: blob.schema_version,
        bytes: blob.bytes.as_ref(),
    })
    .unwrap();

    assert_eq!(restored.generation(), 0);
    assert_eq!(restored.get().value, vec![1, 2, 3]);
}
