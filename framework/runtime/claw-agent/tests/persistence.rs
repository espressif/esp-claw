mod support;

use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use claw_agent::{AgentSystem, SessionEvent, SessionId, TurnCause, TurnId};
use claw_checkpoint::{Checkpoint, CheckpointStorage, FsCheckpointStorage};
use claw_interface::{
    BlockingHttpAdapter, Cancel, ClawFs, ClawHttp, DiskFs, HttpError, HttpJsonRequest,
    HttpResponse, HttpResponseFuture, HttpStatusCode, ImmediateTimer, MemFs, SharedScriptHttp,
    StdThread, TokioExecutor,
};
use claw_tool::{SyncToolHandler, Tool, ToolInvocation, ToolOutput, ToolResult, ToolSpec};
use futures_lite::future::block_on;
use serde_json::Value;
use support::{
    assistant_text, build_mem_system, drain_until_turn_ended, install_script, llm_config, mem_root,
    persistence, serialize_script,
};
use tempdir::TempDir;

type DiskAgentSystem = AgentSystem<DiskFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;
type BlockingDiskAgentSystem = AgentSystem<DiskFs, BlockingPendingHttp, ImmediateTimer>;

static HOLD_BLOCKING_HTTP: AtomicBool = AtomicBool::new(false);

#[derive(Default)]
struct BlockingPendingHttp;

impl ClawHttp for BlockingPendingHttp {
    fn post_json<'a>(
        &'a mut self,
        _request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            while HOLD_BLOCKING_HTTP.load(Ordering::Relaxed) {
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                std::thread::sleep(Duration::from_millis(10));
            }
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body: assistant_text("first system completed"),
            })
        })
    }
}

struct BlockingHttpHold;

impl BlockingHttpHold {
    fn hold() -> Self {
        HOLD_BLOCKING_HTTP.store(true, Ordering::Relaxed);
        Self
    }
}

impl Drop for BlockingHttpHold {
    fn drop(&mut self) {
        HOLD_BLOCKING_HTTP.store(false, Ordering::Relaxed);
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
fn pending_input_restores_and_runs_after_rebuild() {
    let _script = serialize_script();
    let root = TempDir::new("claw-agent-pending-input").unwrap();
    let root = root.path().to_string_lossy().into_owned();

    let first_system = build_blocking_disk_system(&root);
    let hold = BlockingHttpHold::hold();
    let session = first_system.new_session();
    let (control, _events) = first_system.open_session(session).unwrap();
    block_on(control.submit("recover me")).unwrap();
    assert_eq!(
        session_drive_pending_text::<DiskFs>(&root, session),
        Some("recover me".to_string())
    );

    let recovered = build_disk_system(&root, vec![assistant_text("recovered")]);
    let (_control, mut events) = recovered.open_session(session).unwrap();
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

    drop(recovered);
    drop(hold);
    drop(first_system);
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

fn build_blocking_disk_system(root: &str) -> BlockingDiskAgentSystem {
    BlockingDiskAgentSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root))
        .unwrap()
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
