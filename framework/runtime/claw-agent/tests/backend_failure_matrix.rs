#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::collections::BTreeMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Duration;

use claw_agent::{AgentError, AgentSystem, SessionEvent};
use claw_interface::{
    Cancel, ClawFile, ClawFs, ClawHttp, ClawTimer, FsError, HttpError, HttpJsonRequest,
    HttpRequestFailure, HttpResponse, HttpResponseFuture, HttpStatusCode, ImmediateTimer, MemFs,
    SleepOutcome, StdThread, TimerFuture, TokioExecutor,
};
use claw_tool::{SyncToolHandler, Tool, ToolInvocation, ToolOutput, ToolResult, ToolSpec};
use futures_lite::future::block_on;
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type PermanentHttpSystem = AgentSystem<MemFs, Sse<PermanentHttp>, CountingTimer>;
type TransientThenSuccessSystem = AgentSystem<MemFs, Sse<TransientThenSuccessHttp>, CountingTimer>;
type TransientExhaustSystem = AgentSystem<MemFs, Sse<TransientOnlyHttp>, CountingTimer>;
type FsReadFailSystem = AgentSystem<AlwaysFailFs, Sse<PermanentHttp>, ImmediateTimer>;
type FsWriteFailSystem = AgentSystem<WriteFailFs, Sse<PermanentHttp>, ImmediateTimer>;

static BACKEND_LOCK: Mutex<()> = Mutex::new(());
static HTTP_CALLS: AtomicUsize = AtomicUsize::new(0);
static TIMER_SLEEPS: AtomicUsize = AtomicUsize::new(0);

#[test]
fn backend_csv_failure_matrix_covers_fs_http_and_timer_failures() {
    let _guard = BACKEND_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/backend_failure_matrix.csv")) {
        reset_counters();
        let case = field(&row, "case");
        let actual_error = match field(&row, "backend") {
            "fs_read_fail" => fs_read_failure(),
            "fs_write_fail" => fs_checkpoint_write_failure(),
            "http_permanent" => http_permanent_failure(),
            "http_transient_then_success" => http_transient_then_success(case),
            "http_transient_exhausts_retries" => http_transient_exhausts_retries(),
            other => panic!("unknown backend case in fixture: {other}"),
        };

        assert_error_contains(
            actual_error.as_deref(),
            field(&row, "expected_error_contains"),
            case,
        );
        assert_eq!(
            HTTP_CALLS.load(Ordering::SeqCst),
            parse_usize(&row, "expected_http_calls"),
            "case {case}: unexpected HTTP call count"
        );
        assert_eq!(
            TIMER_SLEEPS.load(Ordering::SeqCst),
            parse_usize(&row, "expected_timer_sleeps"),
            "case {case}: unexpected timer sleep count"
        );
    }
}

#[derive(Default)]
struct PermanentHttp;

impl ClawHttp for PermanentHttp {
    fn post_json<'a>(
        &'a mut self,
        _request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            HTTP_CALLS.fetch_add(1, Ordering::SeqCst);
            Err(HttpError::InvalidUrl)
        })
    }
}

#[derive(Default)]
struct TransientThenSuccessHttp;

impl ClawHttp for TransientThenSuccessHttp {
    fn post_json<'a>(
        &'a mut self,
        _request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            let call = HTTP_CALLS.fetch_add(1, Ordering::SeqCst);
            if call == 0 {
                Err(HttpError::RequestFailed(HttpRequestFailure::transport(
                    "temporary backend outage",
                )))
            } else {
                Ok(HttpResponse {
                    status_code: HttpStatusCode::OK,
                    body: assistant_text("recovered-after-retry"),
                })
            }
        })
    }
}

#[derive(Default)]
struct TransientOnlyHttp;

impl ClawHttp for TransientOnlyHttp {
    fn post_json<'a>(
        &'a mut self,
        _request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            HTTP_CALLS.fetch_add(1, Ordering::SeqCst);
            Err(HttpError::RequestFailed(HttpRequestFailure::transport(
                "retry backoff should be cancelled",
            )))
        })
    }
}

#[derive(Default)]
struct CountingTimer;

impl ClawTimer for CountingTimer {
    fn sleep<'a>(&'a mut self, _duration: Duration, cancel: Cancel<'a>) -> TimerFuture<'a> {
        Box::pin(async move {
            TIMER_SLEEPS.fetch_add(1, Ordering::SeqCst);
            if cancel.is_cancelled() {
                SleepOutcome::Cancelled
            } else {
                SleepOutcome::Completed
            }
        })
    }
}

#[derive(Default)]
struct FailingFile;

impl ClawFile for FailingFile {
    fn read_to_end(&mut self) -> Result<Vec<u8>, FsError> {
        Err(FsError::io_message("read failed"))
    }

    fn read_exact_at(&mut self, _offset: u64, _len: usize) -> Result<Vec<u8>, FsError> {
        Err(FsError::io_message("read_at failed"))
    }

    fn size(&self) -> Result<u64, FsError> {
        Err(FsError::io_message("size failed"))
    }

    fn write_all(&mut self, _data: &[u8]) -> Result<(), FsError> {
        Err(FsError::io_message("write failed"))
    }
}

struct AlwaysFailFs;

impl ClawFs for AlwaysFailFs {
    type File = FailingFile;

    fn open(_path: &str) -> Result<Self::File, FsError> {
        Err(FsError::io_message("open failed"))
    }

    fn create(_path: &str) -> Result<Self::File, FsError> {
        Err(FsError::io_message("create failed"))
    }

    fn open_append(_path: &str) -> Result<Self::File, FsError> {
        Err(FsError::io_message("append failed"))
    }

    fn rename(_from: &str, _to: &str) -> Result<(), FsError> {
        Err(FsError::io_message("rename failed"))
    }

    fn create_dir_all(_path: &str) -> Result<(), FsError> {
        Err(FsError::io_message("mkdir failed"))
    }

    fn exists(_path: &str) -> bool {
        false
    }

    fn remove(_path: &str) -> Result<(), FsError> {
        Err(FsError::io_message("remove failed"))
    }

    fn list_dir(_path: &str) -> Result<Vec<String>, FsError> {
        Err(FsError::io_message("list failed"))
    }
}

struct WriteFailFs;

impl ClawFs for WriteFailFs {
    type File = FailingFile;

    fn open(_path: &str) -> Result<Self::File, FsError> {
        Err(FsError::NotFound)
    }

    fn create(_path: &str) -> Result<Self::File, FsError> {
        Err(FsError::io_message("checkpoint create failed"))
    }

    fn open_append(_path: &str) -> Result<Self::File, FsError> {
        Err(FsError::io_message("append failed"))
    }

    fn rename(_from: &str, _to: &str) -> Result<(), FsError> {
        Err(FsError::io_message("rename failed"))
    }

    fn create_dir_all(_path: &str) -> Result<(), FsError> {
        Err(FsError::io_message("checkpoint mkdir failed"))
    }

    fn exists(_path: &str) -> bool {
        false
    }

    fn remove(_path: &str) -> Result<(), FsError> {
        Ok(())
    }

    fn list_dir(_path: &str) -> Result<Vec<String>, FsError> {
        Ok(Vec::new())
    }
}

struct CheckpointTool;

impl ToolSpec for CheckpointTool {
    fn name(&self) -> &str {
        "checkpoint_probe"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"checkpoint_probe"}}"#
    }
}

impl SyncToolHandler for CheckpointTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        Ok(ToolOutput {
            output: call.arguments_json().to_owned(),
            ok: true,
        })
    }
}

fn fs_read_failure() -> Option<String> {
    build_fs_read_fail_system()
        .err()
        .map(|error| error.to_string())
}

fn fs_checkpoint_write_failure() -> Option<String> {
    let system = build_fs_write_fail_system().unwrap();
    system
        .tool_registry()
        .register(Tool::from_sync(CheckpointTool))
        .err()
        .map(|error| error.to_string())
}

fn http_permanent_failure() -> Option<String> {
    MemFs::new();
    let system = PermanentHttpSystem::new::<StdThread, TokioExecutor>(
        llm_config(),
        persistence(&mem_root("http-permanent")),
    )
    .unwrap();
    first_failure_text(drive_one_turn(&system, "permanent failure"))
}

fn http_transient_then_success(case: &str) -> Option<String> {
    MemFs::new();
    let system = TransientThenSuccessSystem::new::<StdThread, TokioExecutor>(
        llm_config(),
        persistence(&mem_root("http-transient-success")),
    )
    .unwrap();
    let events = drive_one_turn(&system, "recover");
    assert_eq!(
        output_fragments(&events),
        vec!["recovered-after-retry".to_string()],
        "case {case}"
    );
    first_failure_text(events)
}

fn http_transient_exhausts_retries() -> Option<String> {
    MemFs::new();
    let system = TransientExhaustSystem::new::<StdThread, TokioExecutor>(
        llm_config(),
        persistence(&mem_root("transient-exhaust")),
    )
    .unwrap();
    first_failure_text(drive_one_turn(&system, "exhaust transient retries"))
}

fn build_fs_read_fail_system() -> Result<FsReadFailSystem, AgentError> {
    FsReadFailSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence("/fs-read-fail"))
}

fn build_fs_write_fail_system() -> Result<FsWriteFailSystem, AgentError> {
    FsWriteFailSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence("/fs-write-fail"))
}

fn drive_one_turn<Filesystem, Http, Timer>(
    system: &AgentSystem<Filesystem, Sse<Http>, Timer>,
    input: &str,
) -> Vec<SessionEvent>
where
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let session = system.new_session();
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit(input)).unwrap();
    drain_until_turn_ended(&mut events)
}

fn first_failure_text(events: Vec<SessionEvent>) -> Option<String> {
    events.into_iter().find_map(|event| match event {
        SessionEvent::Error { message } => Some(message),
        SessionEvent::Output { text } if text.contains("[failed:") => Some(text),
        _ => None,
    })
}

fn output_fragments(events: &[SessionEvent]) -> Vec<String> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::Output { text } => Some(text.clone()),
            _ => None,
        })
        .collect()
}

fn assert_error_contains(actual: Option<&str>, expected_contains: &str, case: &str) {
    if expected_contains.is_empty() {
        assert!(actual.is_none(), "case {case}: unexpected error {actual:?}");
    } else {
        let actual = actual.unwrap_or_else(|| panic!("case {case}: expected error"));
        assert!(
            actual.contains(expected_contains),
            "case {case}: expected {actual:?} to contain {expected_contains:?}"
        );
    }
}

fn reset_counters() {
    HTTP_CALLS.store(0, Ordering::SeqCst);
    TIMER_SLEEPS.store(0, Ordering::SeqCst);
}

fn parse_usize(row: &BTreeMap<String, String>, field_name: &str) -> usize {
    field(row, field_name).parse::<usize>().unwrap()
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}
