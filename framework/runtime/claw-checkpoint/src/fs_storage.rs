use std::collections::{BTreeMap, BTreeSet};
use std::marker::PhantomData;

use claw_interface::{ClawFile, ClawFs, FsError};
use serde::{Deserialize, Serialize};

use crate::{
    BatchCheckpoint, BatchId, BatchWrite, ChangePatternHint, Checkpoint, CheckpointStep,
    CheckpointStorage, CheckpointStorageError, CheckpointWrite, LoadCheckpointError, LoadedPart,
    PartStateBlob, PartWrite, StorageHint, StorageSizeHint,
};

const MANIFEST_FILE: &str = "manifest.json";
const INDEX_FILE: &str = "index.json";

pub struct FsCheckpointStorage<F: ClawFs> {
    root: String,
    _filesystem: PhantomData<fn() -> F>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct Manifest {
    latest_step: Option<CheckpointStep>,
    history: Vec<CheckpointStep>,
}

impl Manifest {
    fn empty() -> Self {
        Self {
            latest_step: None,
            history: Vec::new(),
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct CheckpointIndex {
    step: CheckpointStep,
    batches: Vec<BatchIndex>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct BatchIndex {
    name: String,
    id: BatchId,
    parts: Vec<PartIndex>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct PartIndex {
    name: String,
    schema_version: u32,
    encoding: PartEncoding,
    object: String,
    len: u64,
    hash: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
enum PartEncoding {
    Full,
    AppendDelta {
        base_object: String,
        base_len: u64,
        base_hash: u64,
    },
}

impl<F: ClawFs> FsCheckpointStorage<F> {
    fn validate_root(&self) -> Result<(), CheckpointStorageError> {
        if self.root.trim().is_empty() {
            Err(CheckpointStorageError::EmptyRoot)
        } else {
            Ok(())
        }
    }

    fn manifest_path(&self) -> String {
        join_path(&self.root, MANIFEST_FILE)
    }

    fn step_dir(&self, step: CheckpointStep) -> String {
        join_path(&self.root, &step_dir_name(step))
    }

    fn object_path(&self, object: &str) -> String {
        join_path(&self.root, object)
    }

    fn read_manifest(&self) -> Result<Option<Manifest>, CheckpointStorageError> {
        self.validate_root()?;
        let path = self.manifest_path();
        let bytes = match F::read(&path) {
            Ok(bytes) => bytes,
            Err(FsError::NotFound) => return Ok(None),
            Err(source) => {
                return Err(CheckpointStorageError::Fs { path, source });
            }
        };
        serde_json::from_slice(&bytes)
            .map(Some)
            .map_err(CheckpointStorageError::DecodeManifest)
    }

    fn write_manifest(&self, manifest: &Manifest) -> Result<(), CheckpointStorageError> {
        self.validate_root()?;
        F::create_dir_all(&self.root).map_err(|source| CheckpointStorageError::Fs {
            path: self.root.clone(),
            source,
        })?;
        let bytes = serde_json::to_vec(manifest).map_err(CheckpointStorageError::EncodeManifest)?;
        let path = self.manifest_path();
        F::write_atomic(&path, &bytes).map_err(|source| CheckpointStorageError::Fs { path, source })
    }

    fn read_index(&self, step: CheckpointStep) -> Result<CheckpointIndex, CheckpointStorageError> {
        let path = join_path(&self.step_dir(step), INDEX_FILE);
        let bytes = F::read(&path).map_err(|source| CheckpointStorageError::Fs {
            path: path.clone(),
            source,
        })?;
        serde_json::from_slice(&bytes).map_err(CheckpointStorageError::DecodeIndex)
    }

    fn read_index_for_load(
        &self,
        step: CheckpointStep,
    ) -> Result<CheckpointIndex, LoadCheckpointError> {
        let path = join_path(&self.step_dir(step), INDEX_FILE);
        let bytes = F::read(&path).map_err(|source| LoadCheckpointError::Fs {
            path: path.clone(),
            source,
        })?;
        serde_json::from_slice(&bytes).map_err(LoadCheckpointError::DecodeIndex)
    }

    fn materialized_index(&self) -> Result<MaterializedIndex, CheckpointStorageError> {
        let Some(step) = self.latest_step()? else {
            return Ok(BTreeMap::new());
        };
        Ok(index_to_materialized(self.read_index(step)?))
    }
}

impl<F: ClawFs> CheckpointStorage for FsCheckpointStorage<F> {
    fn new(root: String) -> Self {
        Self {
            root,
            _filesystem: PhantomData,
        }
    }

    fn latest_step(&self) -> Result<Option<CheckpointStep>, CheckpointStorageError> {
        Ok(self
            .read_manifest()?
            .and_then(|manifest| manifest.latest_step))
    }

    fn next_step(&mut self) -> Result<CheckpointStep, CheckpointStorageError> {
        let mut latest = 0;
        if let Some(step) = self.latest_step()? {
            latest = step;
        }
        Ok(latest.saturating_add(1))
    }

    fn write_checkpoint(
        &mut self,
        checkpoint: CheckpointWrite<'_>,
    ) -> Result<(), CheckpointStorageError> {
        self.write_checkpoint_with_removals(checkpoint, &[])
    }

    fn write_checkpoint_with_removals(
        &mut self,
        checkpoint: CheckpointWrite<'_>,
        removed_batches: &[crate::BatchKey],
    ) -> Result<(), CheckpointStorageError> {
        self.validate_root()?;
        if let Some(latest) = self.latest_step()? {
            if checkpoint.step <= latest {
                return Err(CheckpointStorageError::NonIncreasingStep {
                    step: checkpoint.step,
                    latest,
                });
            }
        }

        F::create_dir_all(&self.root).map_err(|source| CheckpointStorageError::Fs {
            path: self.root.clone(),
            source,
        })?;
        let step_dir = self.step_dir(checkpoint.step);
        F::create_dir_all(&step_dir).map_err(|source| CheckpointStorageError::Fs {
            path: step_dir.clone(),
            source,
        })?;

        let mut materialized = self.materialized_index()?;
        let written_batches = checkpoint
            .batches
            .iter()
            .map(|batch| batch.batch)
            .collect::<BTreeSet<_>>();
        for removed in removed_batches {
            if written_batches.contains(removed) {
                return Err(CheckpointStorageError::ConflictingBatchMutation {
                    batch: removed.0,
                    id: removed.1,
                });
            }
            let name = checked_segment(removed.0)?;
            materialized.remove(&(name.to_owned(), removed.1));
        }
        for batch in checkpoint.batches {
            apply_batch_write::<F>(&self.root, checkpoint.step, batch, &mut materialized)?;
        }

        let index = materialized_to_index(checkpoint.step, materialized);
        let index_bytes =
            serde_json::to_vec(&index).map_err(CheckpointStorageError::EncodeIndex)?;
        let index_path = join_path(&step_dir, INDEX_FILE);
        F::write_atomic(&index_path, &index_bytes).map_err(|source| {
            CheckpointStorageError::Fs {
                path: index_path,
                source,
            }
        })?;

        let mut manifest = match self.read_manifest()? {
            Some(manifest) => manifest,
            None => Manifest::empty(),
        };
        manifest.latest_step = Some(checkpoint.step);
        manifest.history.retain(|step| *step != checkpoint.step);
        manifest.history.push(checkpoint.step);
        manifest.history.sort_unstable();
        self.write_manifest(&manifest)
    }

    fn load_checkpoint(&self, step: CheckpointStep) -> Result<Checkpoint, LoadCheckpointError> {
        let manifest = self.read_manifest_for_load(step)?;
        if !manifest.history.contains(&step) {
            return Err(LoadCheckpointError::StepNotFound(step));
        }
        let index = self.read_index_for_load(step)?;
        let mut batches = Vec::with_capacity(index.batches.len());
        for batch in index.batches {
            let mut parts = Vec::with_capacity(batch.parts.len());
            for part in batch.parts {
                let bytes = self.read_part_bytes_for_load(&part)?;
                parts.push(LoadedPart {
                    name: part.name,
                    state: PartStateBlob {
                        schema_version: part.schema_version,
                        bytes: std::borrow::Cow::Owned(bytes),
                    },
                });
            }
            batches.push(BatchCheckpoint {
                name: batch.name,
                id: batch.id,
                parts,
            });
        }
        Ok(Checkpoint { step, batches })
    }

    fn prune_history(&mut self, max_history: CheckpointStep) -> Result<(), CheckpointStorageError> {
        if max_history == 0 {
            return Err(CheckpointStorageError::InvalidHistoryRetention);
        }
        let Some(mut manifest) = self.read_manifest()? else {
            return Ok(());
        };

        let original_history = manifest.history.clone();
        manifest.history.sort_unstable();
        manifest.history.dedup();
        let keep = usize::try_from(max_history).unwrap_or(usize::MAX);
        if manifest.history.len() <= keep {
            if manifest.history != original_history {
                self.write_manifest(&manifest)?;
            }
            return Ok(());
        }

        let split = manifest.history.len().saturating_sub(keep);
        let removed = manifest.history.drain(..split).collect::<Vec<_>>();
        let retained = manifest.history.clone();
        manifest.latest_step = retained.last().copied();

        let retained_objects = self.referenced_objects(&retained)?;
        let removed_objects = self.referenced_objects(&removed)?;
        self.write_manifest(&manifest)?;

        let mut cleanup_dirs = BTreeSet::new();
        for step in &removed {
            let step_dir = self.step_dir(*step);
            let index_path = join_path(&step_dir, INDEX_FILE);
            F::remove(&index_path).map_err(|source| CheckpointStorageError::Fs {
                path: index_path,
                source,
            })?;
            cleanup_dirs.insert(step_dir);
        }
        for object in removed_objects {
            if retained_objects.contains(&object) {
                continue;
            }
            let path = self.object_path(&object);
            F::remove(&path).map_err(|source| CheckpointStorageError::Fs {
                path: path.clone(),
                source,
            })?;
            collect_parent_dirs(&self.root, &path, &mut cleanup_dirs);
        }
        self.remove_empty_dirs(cleanup_dirs)
    }
}

impl<F: ClawFs> FsCheckpointStorage<F> {
    fn read_manifest_for_load(
        &self,
        step: CheckpointStep,
    ) -> Result<Manifest, LoadCheckpointError> {
        self.validate_root_for_load()?;
        let path = self.manifest_path();
        let bytes = F::read(&path).map_err(|source| match source {
            FsError::NotFound => LoadCheckpointError::StepNotFound(step),
            source => LoadCheckpointError::Fs {
                path: path.clone(),
                source,
            },
        })?;
        serde_json::from_slice(&bytes).map_err(LoadCheckpointError::DecodeManifest)
    }

    fn validate_root_for_load(&self) -> Result<(), LoadCheckpointError> {
        if self.root.trim().is_empty() {
            Err(LoadCheckpointError::EmptyRoot)
        } else {
            Ok(())
        }
    }

    fn read_part_bytes_for_load(&self, part: &PartIndex) -> Result<Vec<u8>, LoadCheckpointError> {
        match &part.encoding {
            PartEncoding::Full => {
                let path = self.object_path(&part.object);
                let bytes = F::read(&path).map_err(|source| LoadCheckpointError::Fs {
                    path: path.clone(),
                    source,
                })?;
                verify_bytes_for_load(path, &bytes, part.len, part.hash)?;
                Ok(bytes)
            }
            PartEncoding::AppendDelta {
                base_object,
                base_len,
                base_hash,
            } => {
                let base_path = self.object_path(base_object);
                let mut bytes = F::read(&base_path).map_err(|source| LoadCheckpointError::Fs {
                    path: base_path.clone(),
                    source,
                })?;
                verify_bytes_for_load(base_path, &bytes, *base_len, *base_hash)?;

                let delta_path = self.object_path(&part.object);
                let delta = F::read(&delta_path).map_err(|source| LoadCheckpointError::Fs {
                    path: delta_path.clone(),
                    source,
                })?;
                bytes.extend_from_slice(&delta);
                verify_bytes_for_load(delta_path, &bytes, part.len, part.hash)?;
                Ok(bytes)
            }
        }
    }

    fn referenced_objects(
        &self,
        steps: &[CheckpointStep],
    ) -> Result<BTreeSet<String>, CheckpointStorageError> {
        let mut objects = BTreeSet::new();
        for step in steps {
            let index = self.read_index(*step)?;
            collect_index_objects(index, &mut objects);
        }
        Ok(objects)
    }

    fn remove_empty_dirs(
        &self,
        cleanup_dirs: BTreeSet<String>,
    ) -> Result<(), CheckpointStorageError> {
        let mut cleanup_dirs = cleanup_dirs.into_iter().collect::<Vec<_>>();
        cleanup_dirs.sort_by_key(|path| std::cmp::Reverse(path.len()));
        for path in cleanup_dirs {
            let entries = match F::list_dir(&path) {
                Ok(entries) => entries,
                Err(FsError::NotFound) => continue,
                Err(source) => {
                    return Err(CheckpointStorageError::Fs { path, source });
                }
            };
            if entries.is_empty() {
                F::remove(&path).map_err(|source| CheckpointStorageError::Fs { path, source })?;
            }
        }
        Ok(())
    }
}

type MaterializedIndex = BTreeMap<(String, BatchId), BTreeMap<String, PartIndex>>;

fn collect_index_objects(index: CheckpointIndex, objects: &mut BTreeSet<String>) {
    for batch in index.batches {
        for part in batch.parts {
            objects.insert(part.object);
            if let PartEncoding::AppendDelta { base_object, .. } = part.encoding {
                objects.insert(base_object);
            }
        }
    }
}

fn apply_batch_write<F: ClawFs>(
    root: &str,
    step: CheckpointStep,
    batch: BatchWrite<'_>,
    materialized: &mut MaterializedIndex,
) -> Result<(), CheckpointStorageError> {
    let batch_name = checked_segment(batch.batch.0)?;
    let batch_id = batch.batch.1;
    let batch_segment = batch_dir_name(batch_name, batch_id);
    let batch_dir = join_path(&join_path(root, &step_dir_name(step)), &batch_segment);
    F::create_dir_all(&batch_dir).map_err(|source| CheckpointStorageError::Fs {
        path: batch_dir.clone(),
        source,
    })?;

    let entry = materialized
        .entry((batch_name.to_owned(), batch_id))
        .or_default();

    for write in batch.writes {
        let part_name = checked_segment(write.name)?;
        let previous = entry.get(part_name);
        let part_index = write_part::<F>(root, step, &batch_segment, part_name, write, previous)?;
        entry.insert(part_name.to_owned(), part_index);
    }
    Ok(())
}

fn write_part<F: ClawFs>(
    root: &str,
    step: CheckpointStep,
    batch_segment: &str,
    part_name: &str,
    write: PartWrite<'_>,
    previous: Option<&PartIndex>,
) -> Result<PartIndex, CheckpointStorageError> {
    if matches!(
        write.hint,
        StorageHint {
            size: StorageSizeHint::Large,
            change: ChangePatternHint::AppendLikely
        }
    ) {
        if let Some(previous) = previous {
            if let Some(part) =
                try_write_append_delta::<F>(root, step, batch_segment, part_name, &write, previous)?
            {
                return Ok(part);
            }
        }
    }
    write_full_part::<F>(root, step, batch_segment, part_name, &write)
}

fn try_write_append_delta<F: ClawFs>(
    root: &str,
    step: CheckpointStep,
    batch_segment: &str,
    part_name: &str,
    write: &PartWrite<'_>,
    previous: &PartIndex,
) -> Result<Option<PartIndex>, CheckpointStorageError> {
    if previous.schema_version != write.state.schema_version {
        return Ok(None);
    }
    if !matches!(previous.encoding, PartEncoding::Full) {
        return Ok(None);
    }

    let previous_path = join_path(root, &previous.object);
    let previous_bytes = F::read(&previous_path).map_err(|source| CheckpointStorageError::Fs {
        path: previous_path.clone(),
        source,
    })?;
    if previous_bytes.len() as u64 != previous.len || hash_bytes(&previous_bytes) != previous.hash {
        return Ok(None);
    }

    let bytes = write.state.bytes.as_ref();
    if !bytes.starts_with(&previous_bytes) {
        return Ok(None);
    }
    let Some(delta) = bytes.get(previous_bytes.len()..) else {
        return Ok(None);
    };

    let object = object_name(step, batch_segment, part_name, "d");
    let path = join_path(root, &object);
    write_object::<F>(&path, delta)?;

    Ok(Some(PartIndex {
        name: part_name.to_owned(),
        schema_version: write.state.schema_version,
        encoding: PartEncoding::AppendDelta {
            base_object: previous.object.clone(),
            base_len: previous.len,
            base_hash: previous.hash,
        },
        object,
        len: bytes.len() as u64,
        hash: hash_bytes(bytes),
    }))
}

fn write_full_part<F: ClawFs>(
    root: &str,
    step: CheckpointStep,
    batch_segment: &str,
    part_name: &str,
    write: &PartWrite<'_>,
) -> Result<PartIndex, CheckpointStorageError> {
    let object = object_name(step, batch_segment, part_name, "f");
    let path = join_path(root, &object);
    let bytes = write.state.bytes.as_ref();
    write_object::<F>(&path, bytes)?;
    Ok(PartIndex {
        name: part_name.to_owned(),
        schema_version: write.state.schema_version,
        encoding: PartEncoding::Full,
        object,
        len: bytes.len() as u64,
        hash: hash_bytes(bytes),
    })
}

fn write_object<F: ClawFs>(path: &str, bytes: &[u8]) -> Result<(), CheckpointStorageError> {
    let mut file = F::create(path).map_err(|source| CheckpointStorageError::Fs {
        path: path.to_owned(),
        source,
    })?;
    file.write_all(bytes)
        .map_err(|source| CheckpointStorageError::Fs {
            path: path.to_owned(),
            source,
        })
}

fn object_name(
    step: CheckpointStep,
    batch_segment: &str,
    part_name: &str,
    extension: &str,
) -> String {
    join_path(
        &join_path(&step_dir_name(step), batch_segment),
        &format!("{part_name}.{extension}"),
    )
}

fn index_to_materialized(index: CheckpointIndex) -> MaterializedIndex {
    let mut materialized = BTreeMap::new();
    for batch in index.batches {
        let mut parts = BTreeMap::new();
        for part in batch.parts {
            parts.insert(part.name.clone(), part);
        }
        materialized.insert((batch.name, batch.id), parts);
    }
    materialized
}

fn materialized_to_index(step: CheckpointStep, materialized: MaterializedIndex) -> CheckpointIndex {
    let batches = materialized
        .into_iter()
        .map(|((name, id), parts)| BatchIndex {
            name,
            id,
            parts: parts.into_values().collect(),
        })
        .collect();
    CheckpointIndex { step, batches }
}

fn verify_bytes_for_load(
    path: String,
    bytes: &[u8],
    expected_len: u64,
    expected_hash: u64,
) -> Result<(), LoadCheckpointError> {
    if bytes.len() as u64 != expected_len || hash_bytes(bytes) != expected_hash {
        return Err(LoadCheckpointError::Integrity { path });
    }
    Ok(())
}

fn checked_segment(segment: &str) -> Result<&str, CheckpointStorageError> {
    let trimmed = segment.trim();
    if trimmed.is_empty()
        || trimmed == "."
        || trimmed == ".."
        || trimmed.contains('/')
        || trimmed.contains('\\')
    {
        return Err(CheckpointStorageError::InvalidName(segment.to_owned()));
    }
    Ok(segment)
}

fn step_dir_name(step: CheckpointStep) -> String {
    format!("step-{step}")
}

fn batch_dir_name(name: &str, id: BatchId) -> String {
    format!("{name}-{id}")
}

fn join_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{child}");
    }
    let parent = parent.trim_end_matches('/');
    if parent.is_empty() {
        child.to_owned()
    } else {
        format!("{parent}/{child}")
    }
}

fn collect_parent_dirs(root: &str, path: &str, dirs: &mut BTreeSet<String>) {
    let mut current = path;
    while let Some((parent, _)) = current.rsplit_once('/') {
        if parent.is_empty() || parent == root {
            break;
        }
        dirs.insert(parent.to_owned());
        current = parent;
    }
}

fn hash_bytes(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}
