//! Checkpoint interfaces for durable runtime state.

mod fs_storage;
mod state;

use std::borrow::Cow;
use std::collections::{HashMap, HashSet};

use claw_utils::define_prefixed_id;

pub use fs_storage::FsCheckpointStorage;
pub use state::{DurableState, DurableStateCodec};

pub type SchemaVersion = u32;
pub type PartGeneration = u64;
pub type PartName = &'static str;
pub type BatchGeneration = u64;
pub type BatchName = &'static str;
pub type BatchKey = (BatchName, BatchId);
pub type CheckpointStep = u64;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartStateBlob<'a> {
    pub schema_version: SchemaVersion,
    pub bytes: Cow<'a, [u8]>,
}

impl<'a> PartStateBlob<'a> {
    pub fn as_slice(&self) -> PartStateSlice<'_> {
        PartStateSlice {
            schema_version: self.schema_version,
            bytes: self.bytes.as_ref(),
        }
    }

    pub fn into_owned(self) -> PartStateBlob<'static> {
        PartStateBlob {
            schema_version: self.schema_version,
            bytes: Cow::Owned(self.bytes.into_owned()),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PartStateSlice<'a> {
    pub schema_version: SchemaVersion,
    pub bytes: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StorageSizeHint {
    Small,
    Large,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChangePatternHint {
    Arbitrary,
    AppendLikely,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StorageHint {
    pub size: StorageSizeHint,
    pub change: ChangePatternHint,
}

pub trait DurablePart {
    fn name(&self) -> PartName;

    fn generation(&self) -> PartGeneration;

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError>;

    fn restore_from_state(state: PartStateSlice<'_>) -> Result<Self, DurablePartError>
    where
        Self: Sized,
    {
        let _ = state;
        Err(DurablePartError::UnsupportedRestore)
    }

    fn storage_hint(&self) -> StorageHint;
}

define_prefixed_id!(BatchId, "batch-", "batch");

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct BatchRef {
    pub key: BatchKey,
    pub generation: BatchGeneration,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DependencyRequirement {
    Equal,
    AtLeast,
    AtMost,
}

pub trait DurableBatch {
    fn name(&self) -> BatchName;

    fn id(&self) -> BatchId;

    fn parts(&self) -> Vec<&dyn DurablePart>;

    fn as_ref(&self) -> BatchRef;

    fn depends_on(&self) -> Vec<(BatchRef, DependencyRequirement)>;

    fn refresh_generation(&mut self);
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PartWrite<'a> {
    pub name: PartName,
    pub state: PartStateBlob<'a>,
    pub hint: StorageHint,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchWrite<'a> {
    pub batch: BatchKey,
    pub writes: Vec<PartWrite<'a>>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CheckpointWrite<'a> {
    pub step: CheckpointStep,
    pub batches: Vec<BatchWrite<'a>>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoadedPart {
    pub name: String,
    pub state: PartStateBlob<'static>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct BatchCheckpoint {
    pub name: String,
    pub id: BatchId,
    pub parts: Vec<LoadedPart>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Checkpoint {
    pub step: CheckpointStep,
    pub batches: Vec<BatchCheckpoint>,
}

pub trait CheckpointStorage {
    fn new(root: String) -> Self
    where
        Self: Sized;

    fn latest_step(&self) -> Result<Option<CheckpointStep>, CheckpointStorageError>;

    fn next_step(&mut self) -> Result<CheckpointStep, CheckpointStorageError>;

    fn write_checkpoint(
        &mut self,
        checkpoint: CheckpointWrite<'_>,
    ) -> Result<(), CheckpointStorageError>;

    fn load_checkpoint(&self, step: CheckpointStep) -> Result<Checkpoint, LoadCheckpointError>;

    fn prune_history(&mut self, max_history: CheckpointStep) -> Result<(), CheckpointStorageError>;
}

#[allow(dead_code)]
pub struct CheckpointCoordinator<S: CheckpointStorage> {
    storage: S,
    batches: Vec<Box<dyn DurableBatch>>,

    checkpoint_interval: CheckpointStep,
    history_checkpoints: CheckpointStep,

    last_physical_checkpoint_tick: Option<CheckpointStep>,
    current_checkpoint_tick: Option<CheckpointStep>,

    clean_batches: HashMap<BatchKey, BatchGeneration>,
    clean_parts: HashMap<(BatchKey, PartName), PartGeneration>,

    heads: HashMap<BatchKey, BatchRef>,
}

impl<S: CheckpointStorage> CheckpointCoordinator<S> {
    pub fn new(
        storage: S,
        checkpoint_interval: CheckpointStep,
        history_checkpoints: CheckpointStep,
    ) -> Result<Self, CheckpointCoordinatorInitError> {
        if checkpoint_interval == 0 {
            return Err(CheckpointCoordinatorInitError::InvalidCheckpointInterval);
        }
        if history_checkpoints == 0 {
            return Err(CheckpointCoordinatorInitError::InvalidHistoryCheckpoints);
        }
        let mut storage = storage;
        storage.latest_step()?;
        storage.prune_history(history_checkpoints)?;
        Ok(Self {
            storage,
            batches: Vec::new(),
            checkpoint_interval,
            history_checkpoints,
            last_physical_checkpoint_tick: None,
            current_checkpoint_tick: None,
            clean_batches: HashMap::new(),
            clean_parts: HashMap::new(),
            heads: HashMap::new(),
        })
    }

    pub fn add_batch(&mut self, batch: Box<dyn DurableBatch>) -> &mut Self {
        self.batches.push(batch);
        self
    }

    pub fn maybe_checkpoint(&mut self) -> Result<(), CheckpointError> {
        let mut current_refs = HashMap::with_capacity(self.batches.len());
        let mut indexes = HashMap::with_capacity(self.batches.len());
        let mut planned = HashSet::new();

        for (index, batch) in self.batches.iter_mut().enumerate() {
            batch.refresh_generation();
            let batch_ref = DurableBatch::as_ref(batch.as_ref());
            indexes.insert(batch_ref.key, index);
            current_refs.insert(batch_ref.key, batch_ref);
            if self.clean_batches.get(&batch_ref.key).copied() != Some(batch_ref.generation) {
                planned.insert(batch_ref.key);
            }
        }

        if planned.is_empty() {
            return Ok(());
        }

        let mut previous_tick = 0;
        if let Some(tick) = self.current_checkpoint_tick {
            previous_tick = tick;
        }
        let current_tick = previous_tick.saturating_add(1);
        self.current_checkpoint_tick = Some(current_tick);
        if let Some(last) = self.last_physical_checkpoint_tick {
            if current_tick.saturating_sub(last) < self.checkpoint_interval {
                return Ok(());
            }
        }

        let mut candidate_heads = self.heads.clone();
        for key in &planned {
            if let Some(batch_ref) = current_refs.get(key).copied() {
                candidate_heads.insert(*key, batch_ref);
            }
        }

        loop {
            let mut added = false;
            let keys: Vec<BatchKey> = planned.iter().copied().collect();
            for key in keys {
                let Some(index) = indexes.get(&key).copied() else {
                    return Err(CheckpointError::MissingManagedBatch {
                        batch: key.0,
                        id: key.1,
                    });
                };
                for (required, requirement) in self.batches[index].depends_on() {
                    if dependency_satisfied(
                        candidate_heads.get(&required.key).copied(),
                        required,
                        requirement,
                    ) {
                        continue;
                    }
                    let Some(current) = current_refs.get(&required.key).copied() else {
                        return Err(CheckpointError::UnsatisfiedDependency {
                            batch: key.0,
                            id: key.1,
                            required,
                            requirement,
                        });
                    };
                    candidate_heads.insert(required.key, current);
                    if !dependency_satisfied(Some(current), required, requirement) {
                        return Err(CheckpointError::UnsatisfiedDependency {
                            batch: key.0,
                            id: key.1,
                            required,
                            requirement,
                        });
                    }
                    if planned.insert(required.key) {
                        added = true;
                    }
                }
            }
            if !added {
                break;
            }
        }

        let step = self.storage.next_step()?;
        let mut writes = Vec::new();
        let mut clean_parts = Vec::new();
        let mut clean_batches = Vec::new();

        for batch in &self.batches {
            let batch_ref = DurableBatch::as_ref(batch.as_ref());
            if !planned.contains(&batch_ref.key) {
                continue;
            }
            let mut part_writes = Vec::new();
            for part in batch.parts() {
                let part_key = (batch_ref.key, part.name());
                let generation = part.generation();
                if self.clean_parts.get(&part_key).copied() != Some(generation) {
                    let state =
                        part.export_state()
                            .map_err(|source| CheckpointError::ExportPart {
                                batch: batch_ref.key.0,
                                id: batch_ref.key.1,
                                part: part.name(),
                                source,
                            })?;
                    part_writes.push(PartWrite {
                        name: part.name(),
                        state,
                        hint: part.storage_hint(),
                    });
                }
                clean_parts.push((part_key, generation));
            }
            clean_batches.push((batch_ref.key, batch_ref.generation));
            writes.push(BatchWrite {
                batch: batch_ref.key,
                writes: part_writes,
            });
        }

        self.storage.write_checkpoint(CheckpointWrite {
            step,
            batches: writes,
        })?;
        self.storage.prune_history(self.history_checkpoints)?;

        for (key, generation) in clean_batches {
            self.clean_batches.insert(key, generation);
        }
        for (key, generation) in clean_parts {
            self.clean_parts.insert(key, generation);
        }
        for (key, batch_ref) in candidate_heads {
            if planned.contains(&key) {
                self.heads.insert(key, batch_ref);
            }
        }
        self.last_physical_checkpoint_tick = Some(current_tick);
        Ok(())
    }
}

fn dependency_satisfied(
    candidate: Option<BatchRef>,
    required: BatchRef,
    requirement: DependencyRequirement,
) -> bool {
    let Some(candidate) = candidate else {
        return false;
    };
    if candidate.key != required.key {
        return false;
    }
    match requirement {
        DependencyRequirement::Equal => candidate.generation == required.generation,
        DependencyRequirement::AtLeast => candidate.generation >= required.generation,
        DependencyRequirement::AtMost => candidate.generation <= required.generation,
    }
}

#[derive(Debug, thiserror::Error)]
pub enum DurablePartError {
    #[error("durable part restore is not supported")]
    UnsupportedRestore,
    #[error("failed to encode durable state: {0}")]
    Encode(#[source] serde_json::Error),
    #[error("failed to decode durable state: {0}")]
    Decode(#[source] serde_json::Error),
    #[error("invalid durable state: {0}")]
    InvalidState(&'static str),
}

#[derive(Debug, thiserror::Error)]
pub enum CheckpointStorageError {
    #[error("checkpoint storage root is empty")]
    EmptyRoot,
    #[error("invalid checkpoint name segment: {0}")]
    InvalidName(String),
    #[error("checkpoint step {step} is not newer than latest step {latest}")]
    NonIncreasingStep {
        step: CheckpointStep,
        latest: CheckpointStep,
    },
    #[error("checkpoint history retention must be at least 1")]
    InvalidHistoryRetention,
    #[error("filesystem error at {path}: {source}")]
    Fs {
        path: String,
        #[source]
        source: claw_interface::FsError,
    },
    #[error("failed to encode checkpoint manifest: {0}")]
    EncodeManifest(#[source] serde_json::Error),
    #[error("failed to decode checkpoint manifest: {0}")]
    DecodeManifest(#[source] serde_json::Error),
    #[error("failed to encode checkpoint index: {0}")]
    EncodeIndex(#[source] serde_json::Error),
    #[error("failed to decode checkpoint index: {0}")]
    DecodeIndex(#[source] serde_json::Error),
}

#[derive(Debug, thiserror::Error)]
pub enum LoadCheckpointError {
    #[error("checkpoint storage root is empty")]
    EmptyRoot,
    #[error("checkpoint step not found: {0}")]
    StepNotFound(CheckpointStep),
    #[error("filesystem error at {path}: {source}")]
    Fs {
        path: String,
        #[source]
        source: claw_interface::FsError,
    },
    #[error("failed to decode checkpoint manifest: {0}")]
    DecodeManifest(#[source] serde_json::Error),
    #[error("failed to decode checkpoint index: {0}")]
    DecodeIndex(#[source] serde_json::Error),
    #[error("unsupported checkpoint part encoding: {0}")]
    UnsupportedEncoding(String),
    #[error("checkpoint object integrity check failed at {path}")]
    Integrity { path: String },
}

#[derive(Debug, thiserror::Error)]
pub enum CheckpointCoordinatorInitError {
    #[error("checkpoint interval must be at least 1")]
    InvalidCheckpointInterval,
    #[error("checkpoint history count must be at least 1")]
    InvalidHistoryCheckpoints,
    #[error(transparent)]
    Storage(#[from] CheckpointStorageError),
}

#[derive(Debug, thiserror::Error)]
pub enum CheckpointError {
    #[error("managed batch is missing: {batch}/{id}")]
    MissingManagedBatch { batch: BatchName, id: BatchId },
    #[error("checkpoint dependency for {batch}/{id} is not satisfied")]
    UnsatisfiedDependency {
        batch: BatchName,
        id: BatchId,
        required: BatchRef,
        requirement: DependencyRequirement,
    },
    #[error("failed to export durable part {part} in {batch}/{id}: {source}")]
    ExportPart {
        batch: BatchName,
        id: BatchId,
        part: PartName,
        #[source]
        source: DurablePartError,
    },
    #[error(transparent)]
    Storage(#[from] CheckpointStorageError),
}
