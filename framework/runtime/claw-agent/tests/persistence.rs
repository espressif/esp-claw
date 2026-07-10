mod support;

use std::borrow::Cow;
use std::collections::VecDeque;
use std::sync::{Mutex, MutexGuard};

use claw_agent::{
    AgentError, AgentSystem, OpenSessionError, SessionEvent, SessionId, TurnCause, TurnId,
};
use claw_checkpoint::{
    BatchId, BatchWrite, ChangePatternHint, Checkpoint, CheckpointStorage, CheckpointWrite,
    FsCheckpointStorage, PartStateBlob, PartWrite, StorageHint, StorageSizeHint,
};
use claw_interface::{
    BlockingHttpAdapter, Cancel, ClawFs, ClawHttp, DiskFs, HttpJsonRequest, HttpResponse,
    HttpResponseFuture, HttpStatusCode, ImmediateTimer, MemFs, SharedScriptHttp, StdThread,
    TokioExecutor,
};
use claw_tool::{SyncToolHandler, Tool, ToolInvocation, ToolOutput, ToolResult, ToolSpec};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{
    assistant_text, build_mem_system, drain_until_turn_ended, install_script, llm_config, mem_root,
    persistence, serialize_script,
};
use tempdir::TempDir;

type DiskAgentSystem = AgentSystem<DiskFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;
type RecordingDiskAgentSystem = AgentSystem<DiskFs, RecordingHttp, ImmediateTimer>;

static RECORDING_HTTP_LOCK: Mutex<()> = Mutex::new(());
static RECORDING_HTTP_REPLIES: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static RECORDING_HTTP_REQUESTS: Mutex<Vec<String>> = Mutex::new(Vec::new());

#[derive(Default)]
struct RecordingHttp;

impl ClawHttp for RecordingHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        Box::pin(async move {
            let response = if is_agent_iteration_request(&body) {
                recording_requests().push(body);
                recording_replies()
                    .pop_front()
                    .expect("recording HTTP script exhausted")
            } else {
                assistant_text("[]")
            };
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body: response,
            })
        })
    }
}

#[test]
fn sessions_restore_from_checkpoint_after_rebuild() {
    let _script = serialize_script();
    MemFs::default();
    let root = mem_root("persist-session-registry");

    let first = {
        let system = build_mem_system(&root, Vec::new());
        let session = system.new_session();
        assert_eq!(system.list_sessions(), vec![session]);
        assert!(MemFs::exists(&format!("{root}/checkpoint/manifest.json")));
        session
    };

    let system = build_mem_system(&root, Vec::new());
    assert_eq!(system.list_sessions(), vec![first]);
    let second = system.new_session();
    assert_eq!(second.0, first.0.saturating_add(1));
    assert_eq!(system.list_sessions(), vec![first, second]);
}

#[test]
fn tool_registry_start_state_writes_checkpoint() {
    let _script = serialize_script();
    MemFs::default();
    let root = mem_root("persist-tool-registry");
    let system = build_mem_system(&root, Vec::new());

    system.start_all().unwrap();
    assert_eq!(tool_registry_started::<MemFs>(&root), Some(true));

    system.stop_all().unwrap();
    assert_eq!(tool_registry_started::<MemFs>(&root), Some(false));
}

#[test]
fn tool_registry_direct_mutations_checkpoint_and_restore() {
    let _script = serialize_script();
    MemFs::default();
    let root = mem_root("persist-tool-registry-direct");
    let tool_name = "checkpoint_echo";

    let disabled_step = {
        let system = build_mem_system(&root, Vec::new());
        system
            .tool_registry()
            .register(Tool::from_sync(CheckpointEchoTool))
            .unwrap();
        assert_eq!(tool_registry_enabled::<MemFs>(&root, tool_name), Some(true));

        system.tool_registry().disable(tool_name).unwrap();
        assert_eq!(
            tool_registry_enabled::<MemFs>(&root, tool_name),
            Some(false)
        );
        latest_checkpoint_step::<MemFs>(&format!("{root}/checkpoint"))
    };

    let system = build_mem_system(&root, Vec::new());
    system
        .tool_registry()
        .register(Tool::from_sync(CheckpointEchoTool))
        .unwrap();
    system.tool_registry().disable(tool_name).unwrap();

    assert_eq!(
        latest_checkpoint_step::<MemFs>(&format!("{root}/checkpoint")),
        disabled_step
    );
    assert_eq!(
        tool_registry_enabled::<MemFs>(&root, tool_name),
        Some(false)
    );
}

#[test]
fn tool_registry_keeps_only_two_checkpoints_across_fifty_four_registrations() {
    let _script = serialize_script();
    MemFs::default();
    let root = mem_root("persist-tool-registry-two-slots");
    let checkpoint_root = format!("{root}/checkpoint");
    let system = build_mem_system(&root, Vec::new());

    for index in 1..=54 {
        system
            .tool_registry()
            .register(Tool::from_sync(NumberedCheckpointTool {
                name: format!("checkpoint-tool-{index}"),
            }))
            .unwrap();
    }

    let storage = FsCheckpointStorage::<MemFs>::new(checkpoint_root.clone());
    assert_eq!(storage.latest_step().unwrap(), Some(54));
    assert!(matches!(
        storage.load_checkpoint(52),
        Err(claw_checkpoint::LoadCheckpointError::StepNotFound(52))
    ));
    assert_eq!(tool_count(&storage.load_checkpoint(53).unwrap()), 53);
    assert_eq!(tool_count(&storage.load_checkpoint(54).unwrap()), 54);

    let mut step_directories = MemFs::list_dir(&checkpoint_root)
        .unwrap()
        .into_iter()
        .filter(|entry| entry.starts_with("step-"))
        .collect::<Vec<_>>();
    step_directories.sort();
    assert_eq!(step_directories, vec!["step-53", "step-54"]);
}

#[test]
fn session_drive_turn_counter_restores_from_disk_checkpoint() {
    let _script = serialize_script();
    let root = TempDir::new("claw-agent-session-drive").unwrap();
    let root = root.path().to_string_lossy().into_owned();
    let checkpoint_manifest = format!("{root}/checkpoint/manifest.json");

    let session = {
        let system = build_disk_system(&root, vec![assistant_text("first")]);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();
        block_on(control.submit("one")).unwrap();
        let events = drain_until_turn_ended(&mut events);
        assert_eq!(
            events.first(),
            Some(&SessionEvent::TurnStarted {
                turn: TurnId(1),
                cause: TurnCause::UserSubmit,
            })
        );
        assert!(DiskFs::exists(&checkpoint_manifest));
        let checkpoint = latest_checkpoint::<DiskFs>(&format!("{root}/checkpoint"));
        let runtime = checkpoint
            .batches
            .iter()
            .find(|batch| batch.name == "session-runtime" && batch.id.0 == session.0)
            .expect("session runtime checkpoint exists");
        let instance = runtime
            .parts
            .iter()
            .find(|part| part.name == "orchestrator-instance")
            .expect("orchestrator instance checkpoint exists");
        let instance_json: Value = serde_json::from_slice(instance.state.bytes.as_ref()).unwrap();
        assert!(!instance_json["agent_parts"]
            .as_array()
            .expect("agent_parts is an array")
            .is_empty());
        session
    };

    let system = build_disk_system(&root, vec![assistant_text("second")]);
    assert_eq!(system.list_sessions(), vec![session]);
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit("two")).unwrap();
    let events = drain_until_turn_ended(&mut events);
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted {
            turn: TurnId(2),
            cause: TurnCause::UserSubmit,
        })
    );
}

#[test]
fn session_transcript_history_survives_disk_rebuild_and_reenters_llm_context() {
    let _lock = RECORDING_HTTP_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let root = TempDir::new("claw-agent-transcript-history").unwrap();
    let root = root.path().to_string_lossy().into_owned();

    let session = {
        install_recording_replies(vec![assistant_text("first persisted reply")]);
        let system = build_recording_disk_system(&root);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();
        block_on(control.submit("first persisted user")).unwrap();
        let events = drain_until_turn_ended(&mut events);
        assert!(events
            .iter()
            .any(|event| matches!(event, SessionEvent::Output { text } if text == "first persisted reply")));
        assert_disk_file_contains(
            &root,
            "sessions/roots/conversation-1.jsonl",
            "first persisted user",
        );
        assert_disk_file_contains(
            &root,
            "sessions/roots/conversation-1.jsonl",
            "first persisted reply",
        );
        session
    };

    install_recording_replies(vec![assistant_text("second reply")]);
    let system = build_recording_disk_system(&root);
    assert_eq!(system.list_sessions(), vec![session]);
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit("second user")).unwrap();
    let events = drain_until_turn_ended(&mut events);

    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted {
            turn: TurnId(2),
            cause: TurnCause::UserSubmit,
        })
    );
    assert!(events
        .iter()
        .any(|event| matches!(event, SessionEvent::Output { text } if text == "second reply")));

    let requests = recording_requests().clone();
    assert_eq!(requests.len(), 1, "expected one visible root LLM request");
    assert_contains(&requests[0], "first persisted user");
    assert_contains(&requests[0], "first persisted reply");
    assert_contains(&requests[0], "second user");
    assert_disk_file_contains(&root, "sessions/roots/conversation-1.jsonl", "second user");
    assert_disk_file_contains(&root, "sessions/roots/conversation-1.jsonl", "second reply");
}

#[test]
fn corrupt_transcript_index_rebuilds_from_data_log_after_disk_rebuild() {
    let _lock = RECORDING_HTTP_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let root = TempDir::new("claw-agent-transcript-index-rebuild").unwrap();
    let root = root.path().to_string_lossy().into_owned();
    let index_path = format!("{root}/sessions/roots/conversation-1.json");

    let session = {
        install_recording_replies(vec![assistant_text("reply before index corruption")]);
        let system = build_recording_disk_system(&root);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();
        block_on(control.submit("user before index corruption")).unwrap();
        let events = drain_until_turn_ended(&mut events);
        assert!(events.iter().any(
            |event| matches!(event, SessionEvent::Output { text } if text == "reply before index corruption")
        ));
        assert_disk_json_parses(&root, "sessions/roots/conversation-1.json");
        DiskFs::write_atomic(&index_path, b"{not valid json").unwrap();
        session
    };

    install_recording_replies(vec![assistant_text("reply after index rebuild")]);
    let system = build_recording_disk_system(&root);
    assert_eq!(system.list_sessions(), vec![session]);
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit("user after index corruption")).unwrap();
    let events = drain_until_turn_ended(&mut events);

    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted {
            turn: TurnId(2),
            cause: TurnCause::UserSubmit,
        })
    );
    assert!(events
        .iter()
        .any(|event| matches!(event, SessionEvent::Output { text } if text == "reply after index rebuild")));

    let requests = recording_requests().clone();
    assert_eq!(requests.len(), 1, "expected one visible root LLM request");
    assert_contains(&requests[0], "user before index corruption");
    assert_contains(&requests[0], "reply before index corruption");
    assert_contains(&requests[0], "user after index corruption");
    assert_disk_json_parses(&root, "sessions/roots/conversation-1.json");
}

#[test]
fn deleted_session_does_not_reappear_after_disk_rebuild() {
    let _script = serialize_script();
    let root = TempDir::new("claw-agent-delete-session-persistence").unwrap();
    let root = root.path().to_string_lossy().into_owned();

    let deleted = {
        let system = build_disk_system(&root, vec![assistant_text("before delete")]);
        let session = system.new_session();
        {
            let (control, mut events) = system.open_session(session).unwrap();
            block_on(control.submit("persist before delete")).unwrap();
            let events = drain_until_turn_ended(&mut events);
            assert!(events.iter().any(
                |event| matches!(event, SessionEvent::Output { text } if text == "before delete")
            ));
        }
        system.delete_session(session).unwrap();
        assert_eq!(system.list_sessions(), Vec::<SessionId>::new());
        assert_session_missing(&system, session);
        let checkpoint = latest_checkpoint::<DiskFs>(&format!("{root}/checkpoint"));
        assert!(!checkpoint
            .batches
            .iter()
            .any(|batch| batch.name == "session-runtime" && batch.id.0 == session.0));
        session
    };

    let system = build_disk_system(&root, Vec::new());
    assert_eq!(system.list_sessions(), Vec::<SessionId>::new());
    assert_session_missing(&system, deleted);
    let next = system.new_session();
    assert_eq!(next.0, deleted.0.saturating_add(1));
    assert_eq!(system.list_sessions(), vec![next]);
}

#[test]
fn pending_input_checkpoint_replays_after_disk_rebuild() {
    let _script = serialize_script();
    let root = TempDir::new("claw-agent-pending-input-checkpoint").unwrap();
    let root = root.path().to_string_lossy().into_owned();
    let session = SessionId(1);

    write_pending_session_checkpoint(&root, session, "recover me");
    assert_eq!(
        session_drive_pending_text::<DiskFs>(&root, session),
        Some("recover me".to_string())
    );

    let system = build_disk_system(&root, vec![assistant_text("recovered")]);
    assert_eq!(system.list_sessions(), vec![session]);
    let (_control, mut events) = system.open_session(session).unwrap();
    let events = drain_until_turn_ended(&mut events);
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted {
            turn: TurnId(1),
            cause: TurnCause::UserSubmit,
        })
    );
    assert!(events
        .iter()
        .any(|event| matches!(event, SessionEvent::Output { text } if text == "recovered")));
    assert_eq!(session_drive_pending_text::<DiskFs>(&root, session), None);
    assert_disk_file_contains(&root, "sessions/roots/conversation-1.jsonl", "recover me");
}

struct CheckpointEchoTool;

impl ToolSpec for CheckpointEchoTool {
    fn name(&self) -> &str {
        "checkpoint_echo"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"checkpoint_echo"}}"#
    }
}

struct NumberedCheckpointTool {
    name: String,
}

impl ToolSpec for NumberedCheckpointTool {
    fn name(&self) -> &str {
        &self.name
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"checkpoint_tool"}}"#
    }
}

impl SyncToolHandler for NumberedCheckpointTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        Ok(ToolOutput {
            output: call.arguments_json().to_owned(),
            ok: true,
        })
    }
}

impl SyncToolHandler for CheckpointEchoTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        Ok(ToolOutput {
            output: call.arguments_json().to_owned(),
            ok: true,
        })
    }
}

fn build_disk_system(root: &str, bodies: Vec<String>) -> DiskAgentSystem {
    install_script(bodies);
    DiskAgentSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root)).unwrap()
}

fn build_recording_disk_system(root: &str) -> RecordingDiskAgentSystem {
    RecordingDiskAgentSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root))
        .unwrap()
}

fn install_recording_replies(replies: Vec<String>) {
    *recording_replies() = replies.into();
    recording_requests().clear();
}

fn assert_disk_file_contains(root: &str, relative: &str, fragment: &str) {
    let path = format!("{root}/{relative}");
    let bytes = DiskFs::read(&path).unwrap_or_else(|error| panic!("{path}: {error}"));
    let text = String::from_utf8(bytes).unwrap_or_else(|error| panic!("{path}: {error}"));
    assert!(
        text.contains(fragment),
        "expected {path} to contain {fragment:?}, got {text:?}"
    );
}

fn assert_disk_json_parses(root: &str, relative: &str) {
    let path = format!("{root}/{relative}");
    let bytes = DiskFs::read(&path).unwrap_or_else(|error| panic!("{path}: {error}"));
    serde_json::from_slice::<Value>(&bytes).unwrap_or_else(|error| panic!("{path}: {error}"));
}

fn assert_contains(text: &str, fragment: &str) {
    assert!(
        text.contains(fragment),
        "expected {text:?} to contain {fragment:?}"
    );
}

fn assert_session_missing(system: &DiskAgentSystem, session: SessionId) {
    assert!(matches!(
        system.open_session(session),
        Err(AgentError::OpenSession(OpenSessionError::SessionNotFound(
            missing
        ))) if missing == session
    ));
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

fn latest_checkpoint<F: ClawFs>(root: &str) -> Checkpoint {
    let storage = FsCheckpointStorage::<F>::new(root.to_string());
    let step = latest_checkpoint_step::<F>(root);
    storage.load_checkpoint(step).unwrap()
}

fn latest_checkpoint_step<F: ClawFs>(root: &str) -> u64 {
    FsCheckpointStorage::<F>::new(root.to_string())
        .latest_step()
        .unwrap()
        .expect("checkpoint manifest has latest step")
}

fn tool_count(checkpoint: &Checkpoint) -> usize {
    let state = checkpoint
        .batches
        .iter()
        .find(|batch| batch.name == "tool-registry")
        .and_then(|batch| batch.parts.iter().find(|part| part.name == "tool-registry"))
        .expect("tool registry checkpoint part exists");
    let value: Value = serde_json::from_slice(state.state.bytes.as_ref()).unwrap();
    value["tools"].as_object().unwrap().len()
}

fn write_pending_session_checkpoint(root: &str, session: SessionId, text: &str) {
    let mut storage = FsCheckpointStorage::<DiskFs>::new(format!("{root}/checkpoint"));
    let step = storage.next_step().unwrap();
    storage
        .write_checkpoint(CheckpointWrite {
            step,
            batches: vec![
                BatchWrite {
                    batch: ("session-registry", BatchId::new(1)),
                    writes: vec![PartWrite {
                        name: "session-store",
                        state: json_state(json!({
                            "sessions": [session],
                            "next_session_id": SessionId(session.0.saturating_add(1)),
                        })),
                        hint: small_arbitrary_hint(),
                    }],
                },
                BatchWrite {
                    batch: ("session-runtime", BatchId::new(session.0)),
                    writes: vec![PartWrite {
                        name: "session-drive",
                        state: json_state(json!({
                            "pending_input": { "text": text },
                            "next_turn_id": TurnId(1),
                        })),
                        hint: small_arbitrary_hint(),
                    }],
                },
            ],
        })
        .unwrap();
}

fn json_state(value: Value) -> PartStateBlob<'static> {
    PartStateBlob {
        schema_version: 1,
        bytes: Cow::Owned(serde_json::to_vec(&value).unwrap()),
    }
}

fn small_arbitrary_hint() -> StorageHint {
    StorageHint {
        size: StorageSizeHint::Small,
        change: ChangePatternHint::Arbitrary,
    }
}

fn tool_registry_started<F: ClawFs>(root: &str) -> Option<bool> {
    tool_registry_state::<F>(root).and_then(|state| state["started"].as_bool())
}

fn tool_registry_enabled<F: ClawFs>(root: &str, name: &str) -> Option<bool> {
    tool_registry_state::<F>(root).and_then(|state| state["tools"].get(name)?.as_bool())
}

fn tool_registry_state<F: ClawFs>(root: &str) -> Option<Value> {
    let checkpoint = latest_checkpoint::<F>(&format!("{root}/checkpoint"));
    let part = checkpoint
        .batches
        .iter()
        .find(|batch| batch.name == "tool-registry")
        .and_then(|batch| batch.parts.iter().find(|part| part.name == "tool-registry"))?;
    serde_json::from_slice(part.state.bytes.as_ref()).ok()
}

fn session_drive_pending_text<F: ClawFs>(root: &str, session: SessionId) -> Option<String> {
    let checkpoint = latest_checkpoint::<F>(&format!("{root}/checkpoint"));
    let part = checkpoint
        .batches
        .iter()
        .find(|batch| batch.name == "session-runtime" && batch.id.0 == session.0)
        .and_then(|batch| batch.parts.iter().find(|part| part.name == "session-drive"))?;
    let state: Value = serde_json::from_slice(part.state.bytes.as_ref()).ok()?;
    state["pending_input"]["text"].as_str().map(str::to_string)
}

fn recording_replies() -> MutexGuard<'static, VecDeque<String>> {
    RECORDING_HTTP_REPLIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn recording_requests() -> MutexGuard<'static, Vec<String>> {
    RECORDING_HTTP_REQUESTS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
