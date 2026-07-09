use std::borrow::Cow;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use claw_checkpoint::{
    BatchGeneration, BatchId, BatchName, BatchRef, ChangePatternHint, CheckpointCoordinator,
    CheckpointStorage, DependencyRequirement, DurableBatch, DurablePart, DurablePartError,
    FsCheckpointStorage, LoadCheckpointError, PartGeneration, PartName, PartStateBlob, StorageHint,
    StorageSizeHint,
};
use claw_interface::MemFs;

#[test]
fn coordinator_prunes_history_after_successful_checkpoint() {
    let generation = Arc::new(AtomicU64::new(0));
    let bytes = Arc::new(Mutex::new(Vec::new()));
    let root = "/coordinator-prune";
    let mut coordinator =
        CheckpointCoordinator::new(FsCheckpointStorage::<MemFs>::new(root.to_string()), 1, 2)
            .unwrap();
    coordinator.add_batch(Box::new(TestBatch::new(
        Arc::clone(&generation),
        Arc::clone(&bytes),
    )));

    for step in 1..=3 {
        generation.store(step, Ordering::Relaxed);
        *bytes.lock().unwrap() = format!("step-{step}").into_bytes();
        coordinator.maybe_checkpoint().unwrap();
    }

    let storage = FsCheckpointStorage::<MemFs>::new(root.to_string());
    assert_eq!(storage.latest_step().unwrap(), Some(3));
    assert!(matches!(
        storage.load_checkpoint(1),
        Err(LoadCheckpointError::StepNotFound(1))
    ));
    assert_eq!(
        storage.load_checkpoint(2).unwrap().batches[0].parts[0]
            .state
            .bytes
            .as_ref(),
        b"step-2"
    );
    assert_eq!(
        storage.load_checkpoint(3).unwrap().batches[0].parts[0]
            .state
            .bytes
            .as_ref(),
        b"step-3"
    );
}

struct TestBatch {
    generation: BatchGeneration,
    part: TestPart,
}

impl TestBatch {
    fn new(generation: Arc<AtomicU64>, bytes: Arc<Mutex<Vec<u8>>>) -> Self {
        Self {
            generation: 0,
            part: TestPart { generation, bytes },
        }
    }
}

impl DurableBatch for TestBatch {
    fn name(&self) -> BatchName {
        "test-batch"
    }

    fn id(&self) -> BatchId {
        BatchId::new(1)
    }

    fn parts(&self) -> Vec<&dyn DurablePart> {
        vec![&self.part]
    }

    fn as_ref(&self) -> BatchRef {
        BatchRef {
            key: (self.name(), self.id()),
            generation: self.generation,
        }
    }

    fn depends_on(&self) -> Vec<(BatchRef, DependencyRequirement)> {
        Vec::new()
    }

    fn refresh_generation(&mut self) {
        self.generation = self.part.generation();
    }
}

struct TestPart {
    generation: Arc<AtomicU64>,
    bytes: Arc<Mutex<Vec<u8>>>,
}

impl DurablePart for TestPart {
    fn name(&self) -> PartName {
        "test-part"
    }

    fn generation(&self) -> PartGeneration {
        self.generation.load(Ordering::Relaxed)
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        Ok(PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(self.bytes.lock().unwrap().clone()),
        })
    }

    fn storage_hint(&self) -> StorageHint {
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        }
    }
}
