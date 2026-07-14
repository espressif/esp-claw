#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::borrow::Cow;
use std::collections::BTreeMap;

use claw_agent::AgentSystem;
use claw_checkpoint::{
    BatchId, BatchWrite, ChangePatternHint, CheckpointStorage, CheckpointWrite,
    FsCheckpointStorage, PartStateBlob, PartWrite, StorageHint, StorageSizeHint,
};
use claw_interface::{
    BlockingHttpAdapter, ClawFs, DiskFs, ImmediateTimer, SharedScriptHttp, StdThread, TokioExecutor,
};
use serde_json::{json, Value};
use support::{csv_dicts, persistence};
use tempdir::TempDir;

type CheckpointSystem =
    AgentSystem<DiskFs, Sse<BlockingHttpAdapter<SharedScriptHttp>>, ImmediateTimer>;

#[test]
fn checkpoint_recovery_csv_matrix_reports_public_startup_errors() {
    for row in csv_dicts(include_str!("fixtures/checkpoint_recovery_cases.csv")) {
        let case = field(&row, "case");
        let temp = TempDir::new("claw-agent-checkpoint-recovery").unwrap();
        let root = temp.path().to_string_lossy().into_owned();
        setup_checkpoint(&root, field(&row, "setup"));

        let error = match CheckpointSystem::new::<StdThread, TokioExecutor>(persistence(&root)) {
            Ok(_) => panic!("case {case}: corrupt checkpoint must reject AgentSystem startup"),
            Err(error) => error.to_string(),
        };

        assert!(
            error.contains(field(&row, "expected_error")),
            "case {case}: expected {:?} in {error:?}",
            field(&row, "expected_error")
        );
    }
}

fn setup_checkpoint(root: &str, setup: &str) {
    match setup {
        "corrupt_manifest" => write_corrupt_manifest(root),
        "corrupt_index" => write_corrupt_index(root),
        "missing_tool_part" => write_checkpoint(root, "tool-registry", BatchId::new(1), Vec::new()),
        "invalid_tool_part" => write_checkpoint(
            root,
            "tool-registry",
            BatchId::new(1),
            vec![invalid_part("tool-registry")],
        ),
        "missing_session_part" => {
            write_checkpoint(root, "session-registry", BatchId::new(1), Vec::new())
        }
        "invalid_session_part" => write_checkpoint(
            root,
            "session-registry",
            BatchId::new(1),
            vec![invalid_part("session-store")],
        ),
        "missing_session_state_part" => write_checkpoint_batches(
            root,
            vec![
                batch(
                    "session-registry",
                    BatchId::new(1),
                    vec![valid_session_store_part()],
                ),
                batch(
                    "session-runtime",
                    BatchId::new(1),
                    vec![invalid_part("multiagent-runtime")],
                ),
            ],
        ),
        "invalid_session_state_part" => write_checkpoint_batches(
            root,
            vec![
                batch(
                    "session-registry",
                    BatchId::new(1),
                    vec![valid_session_store_part()],
                ),
                batch(
                    "session-runtime",
                    BatchId::new(1),
                    vec![invalid_part("session-state"), valid_multiagent_part()],
                ),
            ],
        ),
        "invalid_multiagent_runtime_part" => write_checkpoint_batches(
            root,
            vec![
                batch(
                    "session-registry",
                    BatchId::new(1),
                    vec![valid_session_store_part()],
                ),
                batch(
                    "session-runtime",
                    BatchId::new(1),
                    vec![
                        valid_session_state_part(),
                        invalid_part("multiagent-runtime"),
                    ],
                ),
            ],
        ),
        other => panic!("unknown checkpoint setup fixture: {other}"),
    }
}

fn write_corrupt_manifest(root: &str) {
    let checkpoint = checkpoint_root(root);
    DiskFs::create_dir_all(&checkpoint).unwrap();
    DiskFs::write_atomic(&format!("{checkpoint}/manifest.json"), b"{not-json").unwrap();
}

fn write_corrupt_index(root: &str) {
    let checkpoint = checkpoint_root(root);
    DiskFs::create_dir_all(&format!("{checkpoint}/step-1")).unwrap();
    DiskFs::write_atomic(
        &format!("{checkpoint}/manifest.json"),
        br#"{"latest_step":1,"history":[1]}"#,
    )
    .unwrap();
    DiskFs::write_atomic(&format!("{checkpoint}/step-1/index.json"), b"{not-json").unwrap();
}

fn write_checkpoint(
    root: &str,
    batch_name: &'static str,
    batch_id: BatchId,
    writes: Vec<PartWrite<'static>>,
) {
    write_checkpoint_batches(root, vec![batch(batch_name, batch_id, writes)]);
}

fn write_checkpoint_batches(root: &str, batches: Vec<BatchWrite<'static>>) {
    let checkpoint = checkpoint_root(root);
    let mut storage = FsCheckpointStorage::<DiskFs>::new(checkpoint);
    storage
        .write_checkpoint(CheckpointWrite { step: 1, batches })
        .unwrap();
}

fn batch(name: &'static str, id: BatchId, writes: Vec<PartWrite<'static>>) -> BatchWrite<'static> {
    BatchWrite {
        batch: (name, id),
        writes,
    }
}

fn invalid_part(name: &'static str) -> PartWrite<'static> {
    PartWrite {
        name,
        state: PartStateBlob {
            schema_version: 1,
            bytes: Cow::Borrowed(b"not-json"),
        },
        hint: StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        },
    }
}

fn valid_session_store_part() -> PartWrite<'static> {
    json_part(
        "session-store",
        json!({
            "sessions": ["session-1"],
            "next_session_id": "session-2",
        }),
    )
}

fn valid_session_state_part() -> PartWrite<'static> {
    let mut part = json_part(
        "session-state",
        json!({
            "active_turn": null,
            "next_turn_id": "turn-1",
            "reasoning_effort": "medium",
            "pending_reasoning_effort": null,
        }),
    );
    part.state.schema_version = 4;
    part
}

fn valid_multiagent_part() -> PartWrite<'static> {
    let mut part = json_part(
        "multiagent-runtime",
        json!({
            "agents": [],
            "ready_queue": [],
            "approvals": [],
            "agent_slots": [],
        }),
    );
    part.state.schema_version = 4;
    part
}

fn json_part(name: &'static str, value: Value) -> PartWrite<'static> {
    PartWrite {
        name,
        state: PartStateBlob {
            schema_version: 1,
            bytes: Cow::Owned(serde_json::to_vec(&value).unwrap()),
        },
        hint: StorageHint {
            size: StorageSizeHint::Small,
            change: ChangePatternHint::Arbitrary,
        },
    }
}

fn checkpoint_root(root: &str) -> String {
    format!("{root}/checkpoint")
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}
