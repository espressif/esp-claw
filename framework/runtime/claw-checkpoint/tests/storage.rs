#![allow(clippy::unwrap_used)]

use std::borrow::Cow;
use std::sync::atomic::{AtomicU64, Ordering};

use claw_checkpoint::{
    BatchId, BatchWrite, ChangePatternHint, CheckpointStorage, CheckpointWrite,
    FsCheckpointStorage, LoadCheckpointError, PartStateBlob, PartWrite, StorageHint,
    StorageSizeHint,
};
use claw_interface::{ClawFs, MemFs};

static ROOT_ID: AtomicU64 = AtomicU64::new(1);

#[test]
fn later_checkpoint_materializes_unchanged_parts() {
    let root = root("materialized");
    let mut storage = FsCheckpointStorage::<MemFs>::new(root.clone());
    let batch = ("session-runtime", BatchId::new(1));

    storage
        .write_checkpoint(CheckpointWrite {
            step: 1,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part("drive", b"drive-v1"), part("agent", b"agent-v1")],
            }],
        })
        .unwrap();

    storage
        .write_checkpoint(CheckpointWrite {
            step: 2,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part("agent", b"agent-v2")],
            }],
        })
        .unwrap();

    let checkpoint = storage.load_checkpoint(2).unwrap();
    assert_eq!(checkpoint.step, 2);
    assert_eq!(checkpoint.batches.len(), 1);
    let batch = &checkpoint.batches[0];
    assert_eq!(batch.name, "session-runtime");
    assert_eq!(batch.id, BatchId::new(1));

    let mut parts = batch
        .parts
        .iter()
        .map(|part| (part.name.as_str(), part.state.bytes.as_ref()))
        .collect::<Vec<_>>();
    parts.sort_by_key(|(name, _)| *name);
    assert_eq!(
        parts,
        vec![
            ("agent", b"agent-v2".as_slice()),
            ("drive", b"drive-v1".as_slice()),
        ]
    );
    assert!(!MemFs::exists(&format!(
        "{root}/step-2/session-runtime-batch-1/drive.f"
    )));
}

#[test]
fn large_append_likely_writes_append_delta_when_previous_is_prefix() {
    let root = root("append-delta");
    let mut storage = FsCheckpointStorage::<MemFs>::new(root.clone());
    let batch = ("session-runtime", BatchId::new(1));

    storage
        .write_checkpoint(CheckpointWrite {
            step: 1,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello", large_append())],
            }],
        })
        .unwrap();
    storage
        .write_checkpoint(CheckpointWrite {
            step: 2,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello world", large_append())],
            }],
        })
        .unwrap();

    assert!(MemFs::exists(&format!(
        "{root}/step-2/session-runtime-batch-1/log.d"
    )));
    assert!(!MemFs::exists(&format!(
        "{root}/step-2/session-runtime-batch-1/log.f"
    )));
    let checkpoint = storage.load_checkpoint(2).unwrap();
    let bytes = checkpoint.batches[0].parts[0].state.bytes.as_ref();
    assert_eq!(bytes, b"hello world");
}

#[test]
fn append_likely_falls_back_to_full_when_previous_is_not_prefix() {
    let root = root("append-fallback");
    let mut storage = FsCheckpointStorage::<MemFs>::new(root.clone());
    let batch = ("session-runtime", BatchId::new(1));

    storage
        .write_checkpoint(CheckpointWrite {
            step: 1,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello", large_append())],
            }],
        })
        .unwrap();
    storage
        .write_checkpoint(CheckpointWrite {
            step: 2,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"world", large_append())],
            }],
        })
        .unwrap();

    assert!(MemFs::exists(&format!(
        "{root}/step-2/session-runtime-batch-1/log.f"
    )));
    assert!(!MemFs::exists(&format!(
        "{root}/step-2/session-runtime-batch-1/log.d"
    )));
    let checkpoint = storage.load_checkpoint(2).unwrap();
    let bytes = checkpoint.batches[0].parts[0].state.bytes.as_ref();
    assert_eq!(bytes, b"world");
}

#[test]
fn append_delta_is_materialized_as_full_on_next_write() {
    let root = root("append-materialized");
    let mut storage = FsCheckpointStorage::<MemFs>::new(root.clone());
    let batch = ("session-runtime", BatchId::new(1));

    storage
        .write_checkpoint(CheckpointWrite {
            step: 1,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello", large_append())],
            }],
        })
        .unwrap();
    storage
        .write_checkpoint(CheckpointWrite {
            step: 2,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello world", large_append())],
            }],
        })
        .unwrap();
    storage
        .write_checkpoint(CheckpointWrite {
            step: 3,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello world again", large_append())],
            }],
        })
        .unwrap();

    assert!(MemFs::exists(&format!(
        "{root}/step-3/session-runtime-batch-1/log.f"
    )));
    assert!(!MemFs::exists(&format!(
        "{root}/step-3/session-runtime-batch-1/log.d"
    )));
    let checkpoint = storage.load_checkpoint(3).unwrap();
    let bytes = checkpoint.batches[0].parts[0].state.bytes.as_ref();
    assert_eq!(bytes, b"hello world again");
}

#[test]
fn prune_history_keeps_latest_steps_and_removes_old_indexes() {
    let root = root("prune");
    let mut storage = FsCheckpointStorage::<MemFs>::new(root.clone());
    let batch = ("session-runtime", BatchId::new(1));

    for step in 1..=3 {
        storage
            .write_checkpoint(CheckpointWrite {
                step,
                batches: vec![BatchWrite {
                    batch,
                    writes: vec![part("drive", format_step(step))],
                }],
            })
            .unwrap();
    }

    storage.prune_history(2).unwrap();

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
    assert!(!MemFs::exists(&format!("{root}/step-1/index.json")));
    assert!(!MemFs::exists(&format!(
        "{root}/step-1/session-runtime-batch-1/drive.f"
    )));
}

#[test]
fn prune_history_preserves_delta_base_used_by_retained_checkpoint() {
    let root = root("prune-delta-base");
    let mut storage = FsCheckpointStorage::<MemFs>::new(root.clone());
    let batch = ("session-runtime", BatchId::new(1));

    storage
        .write_checkpoint(CheckpointWrite {
            step: 1,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello", large_append())],
            }],
        })
        .unwrap();
    storage
        .write_checkpoint(CheckpointWrite {
            step: 2,
            batches: vec![BatchWrite {
                batch,
                writes: vec![part_with_hint("log", b"hello world", large_append())],
            }],
        })
        .unwrap();

    storage.prune_history(1).unwrap();

    assert!(matches!(
        storage.load_checkpoint(1),
        Err(LoadCheckpointError::StepNotFound(1))
    ));
    assert_eq!(
        storage.load_checkpoint(2).unwrap().batches[0].parts[0]
            .state
            .bytes
            .as_ref(),
        b"hello world"
    );
    assert!(!MemFs::exists(&format!("{root}/step-1/index.json")));
    assert!(MemFs::exists(&format!(
        "{root}/step-1/session-runtime-batch-1/log.f"
    )));
    assert!(MemFs::exists(&format!(
        "{root}/step-2/session-runtime-batch-1/log.d"
    )));
}

fn root(name: &str) -> String {
    let id = ROOT_ID.fetch_add(1, Ordering::Relaxed);
    format!("/checkpoint-{name}-{id}")
}

fn small_full() -> StorageHint {
    StorageHint {
        size: StorageSizeHint::Small,
        change: ChangePatternHint::Arbitrary,
    }
}

fn large_append() -> StorageHint {
    StorageHint {
        size: StorageSizeHint::Large,
        change: ChangePatternHint::AppendLikely,
    }
}

fn part(name: &'static str, bytes: &'static [u8]) -> PartWrite<'static> {
    part_with_hint(name, bytes, small_full())
}

fn part_with_hint(
    name: &'static str,
    bytes: &'static [u8],
    hint: StorageHint,
) -> PartWrite<'static> {
    PartWrite {
        name,
        state: PartStateBlob {
            schema_version: 1,
            bytes: Cow::Borrowed(bytes),
        },
        hint,
    }
}

fn format_step(step: u64) -> &'static [u8] {
    match step {
        1 => b"step-1",
        2 => b"step-2",
        3 => b"step-3",
        _ => b"step-other",
    }
}
