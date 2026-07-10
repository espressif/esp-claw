#![allow(clippy::unwrap_used)]

use std::borrow::Cow;
use std::collections::BTreeSet;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Barrier, Mutex};

use claw_checkpoint::{
    BatchGeneration, BatchId, BatchName, BatchRef, ChangePatternHint, Checkpoint,
    CheckpointCoordinator, CheckpointError, CheckpointStep, CheckpointStorage,
    CheckpointStorageError, CheckpointWrite, DependencyRequirement, DurableBatch,
    DurableBatchSnapshot, DurablePart, DurablePartError, DurablePartSnapshot, FsCheckpointStorage,
    LoadCheckpointError, PartGeneration, PartName, PartStateBlob, SharedCheckpointCoordinator,
    StorageHint, StorageSizeHint,
};
use claw_interface::{ClawFs, DiskFs, FsError, MemFs};
use tempdir::TempDir;

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

#[test]
fn shared_coordinator_keeps_two_slots_across_fifty_four_updates() {
    let temp = TempDir::new("claw-checkpoint-two-slots").unwrap();
    let root = temp
        .path()
        .join("checkpoint")
        .to_string_lossy()
        .into_owned();
    let coordinator =
        SharedCheckpointCoordinator::new(TwoSlotStorage::for_root(root.clone()), 1, 2).unwrap();

    for generation in 1..=54 {
        coordinator
            .checkpoint(vec![batch_snapshot(
                "tool-registry",
                1,
                vec![part_snapshot(
                    "tool-registry",
                    generation,
                    format!("tool-count-{generation}").as_bytes(),
                )],
            )])
            .unwrap();

        let expected_history = if generation == 1 {
            vec![1]
        } else {
            vec![generation - 1, generation]
        };
        assert_eq!(manifest_history(&root), expected_history);
        assert_eq!(step_directories(&root), step_names(&expected_history));
    }

    let storage = FsCheckpointStorage::<DiskFs>::new(root);
    assert!(matches!(
        storage.load_checkpoint(52),
        Err(LoadCheckpointError::StepNotFound(52))
    ));
    assert_eq!(
        part_bytes(&storage.load_checkpoint(53).unwrap()),
        b"tool-count-53"
    );
    assert_eq!(
        part_bytes(&storage.load_checkpoint(54).unwrap()),
        b"tool-count-54"
    );
}

#[test]
fn shared_coordinator_does_not_cache_a_failed_candidate() {
    let temp = TempDir::new("claw-checkpoint-failed-candidate").unwrap();
    let root = temp
        .path()
        .join("checkpoint")
        .to_string_lossy()
        .into_owned();
    let coordinator =
        SharedCheckpointCoordinator::new(FailOnceStorage::for_root(root.clone()), 1, 2).unwrap();

    let failed = coordinator.checkpoint(vec![batch_snapshot(
        "tool-registry",
        1,
        vec![part_snapshot("tool-registry", 1, b"must-not-leak")],
    )]);
    assert!(failed.is_err());

    coordinator
        .checkpoint(vec![batch_snapshot(
            "session-registry",
            1,
            vec![part_snapshot("session-store", 1, b"session-state")],
        )])
        .unwrap();

    let storage = FsCheckpointStorage::<DiskFs>::new(root);
    let checkpoint = storage.load_checkpoint(1).unwrap();
    assert_eq!(
        checkpoint
            .batches
            .iter()
            .map(|batch| batch.name.as_str())
            .collect::<Vec<_>>(),
        vec!["session-registry"]
    );
}

#[test]
fn shared_coordinator_rejects_an_atomic_batch_with_a_stale_part() {
    let temp = TempDir::new("claw-checkpoint-stale-batch").unwrap();
    let root = temp
        .path()
        .join("checkpoint")
        .to_string_lossy()
        .into_owned();
    let coordinator =
        SharedCheckpointCoordinator::new(FsCheckpointStorage::<DiskFs>::new(root.clone()), 1, 2)
            .unwrap();

    coordinator
        .checkpoint(vec![batch_snapshot(
            "session-runtime",
            7,
            vec![
                part_snapshot("drive", 2, b"drive-v2"),
                part_snapshot("instance", 2, b"instance-v2"),
            ],
        )])
        .unwrap();
    let stale = coordinator.checkpoint(vec![batch_snapshot(
        "session-runtime",
        7,
        vec![
            part_snapshot("drive", 3, b"drive-v3"),
            part_snapshot("instance", 1, b"instance-v1-stale"),
        ],
    )]);

    assert!(matches!(
        stale,
        Err(CheckpointError::SnapshotGenerationConflict { .. })
    ));

    let storage = FsCheckpointStorage::<DiskFs>::new(root);
    assert_eq!(storage.latest_step().unwrap(), Some(1));
    let checkpoint = storage.load_checkpoint(1).unwrap();
    let batch = checkpoint
        .batches
        .iter()
        .find(|batch| batch.name == "session-runtime")
        .unwrap();
    let parts = batch
        .parts
        .iter()
        .map(|part| (part.name.as_str(), part.state.bytes.as_ref()))
        .collect::<std::collections::BTreeMap<_, _>>();
    assert_eq!(parts["drive"], b"drive-v2");
    assert_eq!(parts["instance"], b"instance-v2");
}

#[test]
fn shared_coordinator_rejects_conflicting_state_at_the_same_generation() {
    let temp = TempDir::new("claw-checkpoint-generation-conflict").unwrap();
    let root = temp
        .path()
        .join("checkpoint")
        .to_string_lossy()
        .into_owned();
    let coordinator =
        SharedCheckpointCoordinator::new(FsCheckpointStorage::<DiskFs>::new(root.clone()), 1, 2)
            .unwrap();

    coordinator
        .checkpoint(vec![batch_snapshot(
            "tool-registry",
            1,
            vec![part_snapshot("tool-registry", 7, b"committed")],
        )])
        .unwrap();
    let conflict = coordinator.checkpoint(vec![batch_snapshot(
        "tool-registry",
        1,
        vec![part_snapshot("tool-registry", 7, b"conflicting")],
    )]);

    assert!(matches!(
        conflict,
        Err(CheckpointError::SnapshotGenerationConflict { .. })
    ));
    let storage = FsCheckpointStorage::<DiskFs>::new(root);
    assert_eq!(storage.latest_step().unwrap(), Some(1));
    assert_eq!(
        part_bytes(&storage.load_checkpoint(1).unwrap()),
        b"committed"
    );
}

#[test]
fn shared_coordinator_tombstones_a_restored_batch_and_prunes_its_objects() {
    let temp = TempDir::new("claw-checkpoint-batch-tombstone").unwrap();
    let root = temp
        .path()
        .join("checkpoint")
        .to_string_lossy()
        .into_owned();
    let coordinator =
        SharedCheckpointCoordinator::new(FsCheckpointStorage::<DiskFs>::new(root.clone()), 1, 2)
            .unwrap();

    coordinator
        .checkpoint(vec![
            batch_snapshot(
                "tool-registry",
                1,
                vec![part_snapshot("tool-registry", 1, b"tool-v1")],
            ),
            batch_snapshot(
                "session-runtime",
                7,
                vec![part_snapshot("state", 1, b"runtime-v1")],
            ),
        ])
        .unwrap();
    drop(coordinator);

    let coordinator =
        SharedCheckpointCoordinator::new(FsCheckpointStorage::<DiskFs>::new(root.clone()), 1, 2)
            .unwrap();
    coordinator
        .remove_batch(("session-runtime", BatchId::new(7)))
        .unwrap();

    let storage = FsCheckpointStorage::<DiskFs>::new(root.clone());
    assert_eq!(manifest_history(&root), vec![1, 2]);
    assert!(has_batch(
        &storage.load_checkpoint(1).unwrap(),
        "session-runtime",
        7
    ));
    assert!(!has_batch(
        &storage.load_checkpoint(2).unwrap(),
        "session-runtime",
        7
    ));

    coordinator
        .checkpoint(vec![batch_snapshot(
            "tool-registry",
            1,
            vec![part_snapshot("tool-registry", 2, b"tool-v2")],
        )])
        .unwrap();

    assert_eq!(manifest_history(&root), vec![2, 3]);
    assert!(matches!(
        storage.load_checkpoint(1),
        Err(LoadCheckpointError::StepNotFound(1))
    ));
    assert!(!has_batch(
        &storage.load_checkpoint(3).unwrap(),
        "session-runtime",
        7
    ));
    assert!(!DiskFs::exists(&format!(
        "{root}/step-1/session-runtime-batch-7/state.f"
    )));
    assert!(!DiskFs::exists(&format!(
        "{root}/step-1/session-runtime-batch-7"
    )));
}

#[test]
fn shared_coordinator_serializes_concurrent_writers_without_losing_batches() {
    const WRITERS: u32 = 16;

    let temp = TempDir::new("claw-checkpoint-concurrent").unwrap();
    let root = temp
        .path()
        .join("checkpoint")
        .to_string_lossy()
        .into_owned();
    let coordinator =
        SharedCheckpointCoordinator::new(FsCheckpointStorage::<DiskFs>::new(root.clone()), 1, 2)
            .unwrap();
    let barrier = Arc::new(Barrier::new(usize::try_from(WRITERS).unwrap() + 1));

    std::thread::scope(|scope| {
        for id in 1..=WRITERS {
            let coordinator = coordinator.clone();
            let barrier = Arc::clone(&barrier);
            scope.spawn(move || {
                let bytes = format!("writer-{id}");
                barrier.wait();
                coordinator
                    .checkpoint(vec![batch_snapshot(
                        "concurrent-writer",
                        id,
                        vec![part_snapshot("state", 1, bytes.as_bytes())],
                    )])
                    .unwrap();
            });
        }
        barrier.wait();
    });

    let storage = FsCheckpointStorage::<DiskFs>::new(root.clone());
    let final_step = u64::from(WRITERS);
    assert_eq!(storage.latest_step().unwrap(), Some(final_step));
    assert_eq!(manifest_history(&root), vec![final_step - 1, final_step]);
    let checkpoint = storage.load_checkpoint(final_step).unwrap();
    let batch_ids = checkpoint
        .batches
        .iter()
        .filter(|batch| batch.name == "concurrent-writer")
        .map(|batch| batch.id.0)
        .collect::<BTreeSet<_>>();
    assert_eq!(batch_ids, (1..=WRITERS).collect());
}

struct TwoSlotStorage {
    root: String,
    inner: FsCheckpointStorage<DiskFs>,
}

impl TwoSlotStorage {
    fn for_root(root: String) -> Self {
        Self {
            inner: FsCheckpointStorage::new(root.clone()),
            root,
        }
    }
}

impl CheckpointStorage for TwoSlotStorage {
    fn new(root: String) -> Self {
        Self::for_root(root)
    }

    fn latest_step(&self) -> Result<Option<CheckpointStep>, CheckpointStorageError> {
        self.inner.latest_step()
    }

    fn next_step(&mut self) -> Result<CheckpointStep, CheckpointStorageError> {
        self.inner.next_step()
    }

    fn write_checkpoint(
        &mut self,
        checkpoint: CheckpointWrite<'_>,
    ) -> Result<(), CheckpointStorageError> {
        if manifest_history_or_empty(&self.root).len() >= 2 {
            return Err(CheckpointStorageError::Fs {
                path: format!("{}/manifest.json", self.root),
                source: FsError::io_message("a third checkpoint would exceed the slot budget"),
            });
        }
        self.inner.write_checkpoint(checkpoint)
    }

    fn load_checkpoint(&self, step: CheckpointStep) -> Result<Checkpoint, LoadCheckpointError> {
        self.inner.load_checkpoint(step)
    }

    fn prune_history(&mut self, max_history: CheckpointStep) -> Result<(), CheckpointStorageError> {
        self.inner.prune_history(max_history)
    }
}

struct FailOnceStorage {
    inner: FsCheckpointStorage<DiskFs>,
    fail_next_write: bool,
}

impl FailOnceStorage {
    fn for_root(root: String) -> Self {
        Self {
            inner: FsCheckpointStorage::new(root),
            fail_next_write: true,
        }
    }
}

impl CheckpointStorage for FailOnceStorage {
    fn new(root: String) -> Self {
        Self::for_root(root)
    }

    fn latest_step(&self) -> Result<Option<CheckpointStep>, CheckpointStorageError> {
        self.inner.latest_step()
    }

    fn next_step(&mut self) -> Result<CheckpointStep, CheckpointStorageError> {
        self.inner.next_step()
    }

    fn write_checkpoint(
        &mut self,
        checkpoint: CheckpointWrite<'_>,
    ) -> Result<(), CheckpointStorageError> {
        if self.fail_next_write {
            self.fail_next_write = false;
            return Err(CheckpointStorageError::Fs {
                path: "injected-write".to_string(),
                source: FsError::io_message("injected first-write failure"),
            });
        }
        self.inner.write_checkpoint(checkpoint)
    }

    fn load_checkpoint(&self, step: CheckpointStep) -> Result<Checkpoint, LoadCheckpointError> {
        self.inner.load_checkpoint(step)
    }

    fn prune_history(&mut self, max_history: CheckpointStep) -> Result<(), CheckpointStorageError> {
        self.inner.prune_history(max_history)
    }
}

fn batch_snapshot(
    name: BatchName,
    id: u32,
    parts: Vec<DurablePartSnapshot>,
) -> DurableBatchSnapshot {
    DurableBatchSnapshot::new(name, BatchId::new(id), parts)
}

fn part_snapshot(name: PartName, generation: PartGeneration, bytes: &[u8]) -> DurablePartSnapshot {
    DurablePartSnapshot::new(
        name,
        generation,
        PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(bytes.to_vec()),
        },
        StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        },
    )
}

fn manifest_history(root: &str) -> Vec<CheckpointStep> {
    let history = manifest_history_or_empty(root);
    assert!(!history.is_empty(), "checkpoint manifest has no history");
    history
}

fn manifest_history_or_empty(root: &str) -> Vec<CheckpointStep> {
    let path = format!("{root}/manifest.json");
    let bytes = match DiskFs::read(&path) {
        Ok(bytes) => bytes,
        Err(FsError::NotFound) => return Vec::new(),
        Err(error) => panic!("failed to read {path}: {error}"),
    };
    let manifest: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    manifest["history"]
        .as_array()
        .unwrap()
        .iter()
        .map(|step| step.as_u64().unwrap())
        .collect()
}

fn step_directories(root: &str) -> Vec<String> {
    let mut entries = DiskFs::list_dir(root).unwrap();
    entries.retain(|entry| entry.starts_with("step-"));
    entries.sort();
    entries
}

fn step_names(steps: &[CheckpointStep]) -> Vec<String> {
    let mut names = steps
        .iter()
        .map(|step| format!("step-{step}"))
        .collect::<Vec<_>>();
    names.sort();
    names
}

fn part_bytes(checkpoint: &Checkpoint) -> &[u8] {
    checkpoint.batches[0].parts[0].state.bytes.as_ref()
}

fn has_batch(checkpoint: &Checkpoint, name: &str, id: u32) -> bool {
    checkpoint
        .batches
        .iter()
        .any(|batch| batch.name == name && batch.id == BatchId::new(id))
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
