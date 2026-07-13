//! Checkpoint interfaces for durable runtime state.

mod fs_storage;
mod state;

use std::borrow::Cow;
use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};

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

pub trait DurableBatch: Send {
    fn name(&self) -> BatchName;

    fn id(&self) -> BatchId;

    fn parts(&self) -> Vec<&dyn DurablePart>;

    fn as_ref(&self) -> BatchRef;

    fn depends_on(&self) -> Vec<(BatchRef, DependencyRequirement)>;

    fn refresh_generation(&mut self);
}

/// Owned snapshot of one [`DurablePart`] captured at a runtime checkpoint
/// boundary.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DurablePartSnapshot {
    name: PartName,
    generation: PartGeneration,
    state: PartStateBlob<'static>,
    hint: StorageHint,
}

impl DurablePartSnapshot {
    pub fn new(
        name: PartName,
        generation: PartGeneration,
        state: PartStateBlob<'static>,
        hint: StorageHint,
    ) -> Self {
        Self {
            name,
            generation,
            state,
            hint,
        }
    }

    /// Capture an owned snapshot without retaining a borrow of the live part.
    pub fn capture(part: &dyn DurablePart) -> Result<Self, DurablePartError> {
        Ok(Self::new(
            part.name(),
            part.generation(),
            part.export_state()?.into_owned(),
            part.storage_hint(),
        ))
    }
}

impl DurablePart for DurablePartSnapshot {
    fn name(&self) -> PartName {
        self.name
    }

    fn generation(&self) -> PartGeneration {
        self.generation
    }

    fn export_state(&self) -> Result<PartStateBlob<'_>, DurablePartError> {
        Ok(PartStateBlob {
            schema_version: self.state.schema_version,
            bytes: Cow::Borrowed(self.state.bytes.as_ref()),
        })
    }

    fn storage_hint(&self) -> StorageHint {
        self.hint
    }
}

/// Owned, thread-safe adapter for a runtime batch whose live state cannot be
/// held by the shared checkpoint coordinator.
#[derive(Clone, Debug)]
pub struct DurableBatchSnapshot {
    name: BatchName,
    id: BatchId,
    generation: BatchGeneration,
    parts: Vec<DurablePartSnapshot>,
    dependencies: Vec<(BatchRef, DependencyRequirement)>,
}

impl DurableBatchSnapshot {
    pub fn new(name: BatchName, id: BatchId, parts: Vec<DurablePartSnapshot>) -> Self {
        Self {
            name,
            id,
            generation: 0,
            parts,
            dependencies: Vec::new(),
        }
    }

    pub fn with_dependencies(
        mut self,
        dependencies: Vec<(BatchRef, DependencyRequirement)>,
    ) -> Self {
        self.dependencies = dependencies;
        self
    }

    fn key(&self) -> BatchKey {
        (self.name, self.id)
    }

    fn set_generation(&mut self, generation: BatchGeneration) {
        self.generation = generation;
    }
}

impl DurableBatch for DurableBatchSnapshot {
    fn name(&self) -> BatchName {
        self.name
    }

    fn id(&self) -> BatchId {
        self.id
    }

    fn parts(&self) -> Vec<&dyn DurablePart> {
        self.parts
            .iter()
            .map(|part| part as &dyn DurablePart)
            .collect()
    }

    fn as_ref(&self) -> BatchRef {
        BatchRef {
            key: self.key(),
            generation: self.generation,
        }
    }

    fn depends_on(&self) -> Vec<(BatchRef, DependencyRequirement)> {
        self.dependencies.clone()
    }

    fn refresh_generation(&mut self) {}
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

    /// Write a checkpoint while removing complete batches from the materialized
    /// checkpoint view.
    ///
    /// Storage implementations that do not support batch tombstones may rely on
    /// this default as long as `removed_batches` is empty.
    fn write_checkpoint_with_removals(
        &mut self,
        checkpoint: CheckpointWrite<'_>,
        removed_batches: &[BatchKey],
    ) -> Result<(), CheckpointStorageError> {
        if !removed_batches.is_empty() {
            return Err(CheckpointStorageError::BatchRemovalUnsupported);
        }
        self.write_checkpoint(checkpoint)
    }

    fn load_checkpoint(&self, step: CheckpointStep) -> Result<Checkpoint, LoadCheckpointError>;

    fn prune_history(&mut self, max_history: CheckpointStep) -> Result<(), CheckpointStorageError>;
}

pub struct CheckpointCoordinator<S: CheckpointStorage> {
    storage: S,
    batches: Vec<Box<dyn DurableBatch>>,
    pending_removed_batches: HashSet<BatchKey>,

    checkpoint_interval: CheckpointStep,
    history_checkpoints: CheckpointStep,

    last_physical_checkpoint_tick: Option<CheckpointStep>,
    current_checkpoint_tick: Option<CheckpointStep>,

    clean_batches: HashMap<BatchKey, BatchGeneration>,
    clean_parts: HashMap<(BatchKey, PartName), PartGeneration>,

    heads: HashMap<BatchKey, BatchRef>,
}

struct BatchMutationUndo {
    key: BatchKey,
    previous: Option<(usize, Box<dyn DurableBatch>)>,
}

#[derive(Clone)]
struct CoordinatorBookkeeping {
    pending_removed_batches: HashSet<BatchKey>,
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
            pending_removed_batches: HashSet::new(),
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
        self.pending_removed_batches
            .remove(&DurableBatch::as_ref(batch.as_ref()).key);
        self.batches.push(batch);
        self
    }

    /// Insert or replace an owned runtime batch snapshot.
    ///
    /// Incoming part generations must never move backwards. Reusing a generation
    /// with different state is also rejected, because accepting it would make
    /// delayed snapshots indistinguishable from the current state.
    pub fn upsert_batch(
        &mut self,
        batch: DurableBatchSnapshot,
    ) -> Result<&mut Self, CheckpointError> {
        let _ = self.apply_upsert_batch(batch)?;
        Ok(self)
    }

    /// Stage removal of a complete batch from the next physical checkpoint.
    ///
    /// Removing an unknown batch is intentional: the batch may exist in the
    /// restored on-disk index even when it has not been added to this coordinator
    /// during the current process lifetime.
    pub fn remove_batch(&mut self, key: BatchKey) -> &mut Self {
        let _ = self.apply_remove_batch(key);
        self
    }

    fn apply_upsert_batch(
        &mut self,
        mut batch: DurableBatchSnapshot,
    ) -> Result<BatchMutationUndo, CheckpointError> {
        let key = batch.key();
        let mut incoming_parts = HashMap::with_capacity(batch.parts.len());
        for part in &batch.parts {
            if incoming_parts.insert(part.name, part).is_some() {
                return Err(CheckpointError::DuplicateSnapshotPart {
                    batch: key.0,
                    id: key.1,
                    part: part.name,
                });
            }
        }

        let Some(index) = self
            .batches
            .iter()
            .position(|existing| existing.as_ref().as_ref().key == key)
        else {
            batch.set_generation(1);
            self.batches.push(Box::new(batch));
            self.pending_removed_batches.remove(&key);
            return Ok(BatchMutationUndo {
                key,
                previous: None,
            });
        };

        let existing = &self.batches[index];
        let mut existing_parts = HashMap::new();
        for part in existing.parts() {
            if existing_parts.insert(part.name(), part).is_some() {
                return Err(CheckpointError::DuplicateSnapshotPart {
                    batch: key.0,
                    id: key.1,
                    part: part.name(),
                });
            }
        }

        let mut changed = incoming_parts.len() != existing_parts.len();
        for (name, existing_part) in &existing_parts {
            let Some(incoming_part) = incoming_parts.get(name).copied() else {
                return Err(CheckpointError::MissingSnapshotPart {
                    batch: key.0,
                    id: key.1,
                    part: name,
                });
            };
            let current = existing_part.generation();
            let incoming = incoming_part.generation;
            if incoming < current {
                return Err(CheckpointError::SnapshotGenerationConflict {
                    batch: key.0,
                    id: key.1,
                    part: name,
                    current,
                    incoming,
                });
            }
            if incoming > current {
                changed = true;
                continue;
            }

            let current_state =
                existing_part
                    .export_state()
                    .map_err(|source| CheckpointError::ExportPart {
                        batch: key.0,
                        id: key.1,
                        part: name,
                        source,
                    })?;
            let same_state = current_state.schema_version == incoming_part.state.schema_version
                && current_state.bytes.as_ref() == incoming_part.state.bytes.as_ref()
                && existing_part.storage_hint() == incoming_part.hint;
            if !same_state {
                return Err(CheckpointError::SnapshotGenerationConflict {
                    batch: key.0,
                    id: key.1,
                    part: name,
                    current,
                    incoming,
                });
            }
        }

        let previous_generation = existing.as_ref().as_ref().generation;
        batch.set_generation(if changed {
            previous_generation.saturating_add(1)
        } else {
            previous_generation
        });
        let previous = std::mem::replace(&mut self.batches[index], Box::new(batch));
        self.pending_removed_batches.remove(&key);
        Ok(BatchMutationUndo {
            key,
            previous: Some((index, previous)),
        })
    }

    fn apply_remove_batch(&mut self, key: BatchKey) -> BatchMutationUndo {
        let previous = self
            .batches
            .iter()
            .position(|batch| DurableBatch::as_ref(batch.as_ref()).key == key)
            .map(|index| (index, self.batches.remove(index)));
        self.pending_removed_batches.insert(key);
        BatchMutationUndo { key, previous }
    }

    fn rollback_batch_mutations(&mut self, mutations: Vec<BatchMutationUndo>) {
        for mutation in mutations.into_iter().rev() {
            if let Some(index) = self
                .batches
                .iter()
                .position(|batch| DurableBatch::as_ref(batch.as_ref()).key == mutation.key)
            {
                self.batches.remove(index);
            }
            if let Some((index, previous)) = mutation.previous {
                self.batches.insert(index.min(self.batches.len()), previous);
            }
        }
    }

    fn bookkeeping(&self) -> CoordinatorBookkeeping {
        CoordinatorBookkeeping {
            pending_removed_batches: self.pending_removed_batches.clone(),
            last_physical_checkpoint_tick: self.last_physical_checkpoint_tick,
            current_checkpoint_tick: self.current_checkpoint_tick,
            clean_batches: self.clean_batches.clone(),
            clean_parts: self.clean_parts.clone(),
            heads: self.heads.clone(),
        }
    }

    fn restore_bookkeeping(&mut self, bookkeeping: CoordinatorBookkeeping) {
        self.pending_removed_batches = bookkeeping.pending_removed_batches;
        self.last_physical_checkpoint_tick = bookkeeping.last_physical_checkpoint_tick;
        self.current_checkpoint_tick = bookkeeping.current_checkpoint_tick;
        self.clean_batches = bookkeeping.clean_batches;
        self.clean_parts = bookkeeping.clean_parts;
        self.heads = bookkeeping.heads;
    }

    pub fn maybe_checkpoint(&mut self) -> Result<(), CheckpointError> {
        self.checkpoint_with_policy(false)
    }

    /// Publish pending changes immediately, bypassing the configured write
    /// interval. Intended for externally observable durability boundaries.
    pub fn checkpoint_now(&mut self) -> Result<(), CheckpointError> {
        self.checkpoint_with_policy(true)
    }

    fn checkpoint_with_policy(&mut self, force: bool) -> Result<(), CheckpointError> {
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

        if planned.is_empty() && self.pending_removed_batches.is_empty() {
            return Ok(());
        }

        let mut previous_tick = 0;
        if let Some(tick) = self.current_checkpoint_tick {
            previous_tick = tick;
        }
        let current_tick = previous_tick.saturating_add(1);
        self.current_checkpoint_tick = Some(current_tick);
        if !force {
            if let Some(last) = self.last_physical_checkpoint_tick {
                if current_tick.saturating_sub(last) < self.checkpoint_interval {
                    return Ok(());
                }
            }
        }

        let mut candidate_heads = self.heads.clone();
        for key in &self.pending_removed_batches {
            candidate_heads.remove(key);
        }
        for key in &planned {
            if let Some(batch_ref) = current_refs.get(key).copied() {
                candidate_heads.insert(*key, batch_ref);
            }
        }

        for batch in &self.batches {
            let batch_ref = DurableBatch::as_ref(batch.as_ref());
            for (required, _) in batch.depends_on() {
                if self.pending_removed_batches.contains(&required.key) {
                    return Err(CheckpointError::RemovedDependency {
                        batch: batch_ref.key.0,
                        id: batch_ref.key.1,
                        required: required.key,
                    });
                }
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

        if self.history_checkpoints > 1 {
            self.storage
                .prune_history(self.history_checkpoints.saturating_sub(1))?;
        }
        let step = self.storage.next_step()?;
        let removed_batches = self
            .pending_removed_batches
            .iter()
            .copied()
            .collect::<Vec<_>>();
        self.storage.write_checkpoint_with_removals(
            CheckpointWrite {
                step,
                batches: writes,
            },
            &removed_batches,
        )?;
        if self.history_checkpoints == 1 {
            self.storage.prune_history(1)?;
        }

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
        for key in removed_batches {
            self.pending_removed_batches.remove(&key);
            self.clean_batches.remove(&key);
            self.clean_parts
                .retain(|(batch_key, _), _| *batch_key != key);
            self.heads.remove(&key);
        }
        self.last_physical_checkpoint_tick = Some(current_tick);
        Ok(())
    }
}

/// Cloneable serialization boundary around one production coordinator.
pub struct SharedCheckpointCoordinator<S: CheckpointStorage> {
    inner: Arc<Mutex<CheckpointCoordinator<S>>>,
}

/// Production coordinator backed by [`FsCheckpointStorage`].
pub type FsCheckpointCoordinator<F> = SharedCheckpointCoordinator<FsCheckpointStorage<F>>;

impl<S: CheckpointStorage> SharedCheckpointCoordinator<S> {
    pub fn new(
        storage: S,
        checkpoint_interval: CheckpointStep,
        history_checkpoints: CheckpointStep,
    ) -> Result<Self, CheckpointCoordinatorInitError> {
        Ok(Self {
            inner: Arc::new(Mutex::new(CheckpointCoordinator::new(
                storage,
                checkpoint_interval,
                history_checkpoints,
            )?)),
        })
    }

    /// Submit one atomic group of owned batch snapshots.
    pub fn checkpoint(&self, batches: Vec<DurableBatchSnapshot>) -> Result<(), CheckpointError> {
        self.checkpoint_and_remove_with_policy(batches, Vec::new(), false)
    }

    /// Submit snapshots and publish them before returning, regardless of the
    /// coordinator's normal write interval.
    pub fn checkpoint_now(
        &self,
        batches: Vec<DurableBatchSnapshot>,
    ) -> Result<(), CheckpointError> {
        self.checkpoint_and_remove_with_policy(batches, Vec::new(), true)
    }

    /// Atomically apply owned snapshots and complete-batch tombstones, publishing
    /// them before returning regardless of the normal write interval.
    ///
    /// If validation, export, pruning, or the checkpoint write fails, the
    /// coordinator's in-memory candidates are restored. The latest published
    /// checkpoint therefore cannot be followed by an unrelated request that
    /// accidentally commits a rejected snapshot.
    pub fn checkpoint_and_remove(
        &self,
        batches: Vec<DurableBatchSnapshot>,
        removed_batches: Vec<BatchKey>,
    ) -> Result<(), CheckpointError> {
        self.checkpoint_and_remove_with_policy(batches, removed_batches, true)
    }

    fn checkpoint_and_remove_with_policy(
        &self,
        batches: Vec<DurableBatchSnapshot>,
        removed_batches: Vec<BatchKey>,
        force: bool,
    ) -> Result<(), CheckpointError> {
        let mut coordinator = self
            .inner
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());

        let mut updated_keys = HashSet::with_capacity(batches.len());
        for batch in &batches {
            let key = batch.key();
            if !updated_keys.insert(key) {
                return Err(CheckpointError::DuplicateBatchMutation {
                    batch: key.0,
                    id: key.1,
                });
            }
        }
        let removed_keys = removed_batches.into_iter().collect::<HashSet<_>>();
        if let Some(key) = updated_keys
            .iter()
            .find(|key| removed_keys.contains(key))
            .copied()
        {
            return Err(CheckpointError::ConflictingBatchMutation {
                batch: key.0,
                id: key.1,
            });
        }

        let bookkeeping = coordinator.bookkeeping();
        let mut mutations = Vec::with_capacity(batches.len() + removed_keys.len());
        for batch in batches {
            match coordinator.apply_upsert_batch(batch) {
                Ok(mutation) => mutations.push(mutation),
                Err(error) => {
                    coordinator.rollback_batch_mutations(mutations);
                    coordinator.restore_bookkeeping(bookkeeping);
                    return Err(error);
                }
            }
        }
        for key in removed_keys {
            mutations.push(coordinator.apply_remove_batch(key));
        }

        let checkpoint_result = if force {
            coordinator.checkpoint_now()
        } else {
            coordinator.maybe_checkpoint()
        };
        if let Err(error) = checkpoint_result {
            coordinator.rollback_batch_mutations(mutations);
            coordinator.restore_bookkeeping(bookkeeping);
            return Err(error);
        }
        Ok(())
    }

    /// Remove one complete batch in a coordinator-managed checkpoint.
    pub fn remove_batch(&self, key: BatchKey) -> Result<(), CheckpointError> {
        self.checkpoint_and_remove(Vec::new(), vec![key])
    }
}

impl<S: CheckpointStorage> Clone for SharedCheckpointCoordinator<S> {
    fn clone(&self) -> Self {
        Self {
            inner: Arc::clone(&self.inner),
        }
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
    #[error("checkpoint storage does not support batch removal")]
    BatchRemovalUnsupported,
    #[error("checkpoint both writes and removes {batch}/{id}")]
    ConflictingBatchMutation { batch: BatchName, id: BatchId },
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
    #[error("duplicate checkpoint mutation for {batch}/{id}")]
    DuplicateBatchMutation { batch: BatchName, id: BatchId },
    #[error("checkpoint both updates and removes {batch}/{id}")]
    ConflictingBatchMutation { batch: BatchName, id: BatchId },
    #[error("duplicate snapshot part {part} in {batch}/{id}")]
    DuplicateSnapshotPart {
        batch: BatchName,
        id: BatchId,
        part: PartName,
    },
    #[error("snapshot is missing existing part {part} in {batch}/{id}")]
    MissingSnapshotPart {
        batch: BatchName,
        id: BatchId,
        part: PartName,
    },
    #[error(
        "snapshot generation conflict for {part} in {batch}/{id}: current {current}, incoming {incoming}"
    )]
    SnapshotGenerationConflict {
        batch: BatchName,
        id: BatchId,
        part: PartName,
        current: PartGeneration,
        incoming: PartGeneration,
    },
    #[error("managed batch is missing: {batch}/{id}")]
    MissingManagedBatch { batch: BatchName, id: BatchId },
    #[error("cannot remove required batch {required:?}; retained batch is {batch}/{id}")]
    RemovedDependency {
        batch: BatchName,
        id: BatchId,
        required: BatchKey,
    },
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
