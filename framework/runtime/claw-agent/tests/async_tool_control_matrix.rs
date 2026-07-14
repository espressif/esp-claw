#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll, Waker};
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::thread;

use claw_agent::{AgentSystem, Message, SessionControlError, SessionEvent, StreamPart, TurnId};
use claw_interface::{
    Cancel, ClawFs, ClawHttp, FsError, HttpError, HttpJsonRequest, HttpResponse,
    HttpResponseFuture, HttpStatusCode, ImmediateTimer, MemFile, MemFs, StdThread, TokioExecutor,
};
use futures_lite::future::{block_on, poll_fn};
use futures_lite::StreamExt;
use serde_json::Value;
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type ControlSystem = AgentSystem<CheckpointFailFs, Sse<ControlHttp>, ImmediateTimer>;

static CONTROL_LOCK: Mutex<()> = Mutex::new(());
static CASE_STATE: Mutex<Option<CaseState>> = Mutex::new(None);
static REQUEST_POLLS: AtomicUsize = AtomicUsize::new(0);
static CONTROL_SENT: AtomicBool = AtomicBool::new(false);
static ALLOW_RESPONSE: AtomicBool = AtomicBool::new(false);
static FAIL_CHECKPOINT_WRITES: AtomicBool = AtomicBool::new(false);
static REQUEST_WAKER: Mutex<Option<Waker>> = Mutex::new(None);

#[test]
fn pending_request_control_ends_the_turn_before_returning() {
    let _lock = CONTROL_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/async_tool_control_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        install_case(fixture.clone());

        let root = mem_root("pending-request-control");
        let system = ControlSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
        system
            .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
            .unwrap();
        let session = system.new_session(claw_agent::SessionPersistence::Persistent);
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(Message::text(format!("run {}", fixture.case)))).unwrap();
        wait_for(
            &REQUEST_POLLS,
            &fixture.case,
            "request did not become pending",
        );

        FAIL_CHECKPOINT_WRITES.store(true, Ordering::SeqCst);
        let control_clone = control.clone();
        let control_name = fixture.control.clone();
        let control_thread = thread::spawn(move || {
            block_on(async move {
                let mut request = core::pin::pin!(async move {
                    match control_name.as_str() {
                        "cancel" => control_clone.cancel().await,
                        "interrupt" => control_clone.interrupt().await,
                        other => panic!("unsupported control {other}"),
                    }
                });
                poll_fn(|cx| match request.as_mut().poll(cx) {
                    Poll::Ready(result) => Poll::Ready(result),
                    Poll::Pending => {
                        CONTROL_SENT.store(true, Ordering::SeqCst);
                        Poll::Pending
                    }
                })
                .await
            })
        });
        wait_for_bool(&CONTROL_SENT, &fixture.case, "control command was not sent");
        ALLOW_RESPONSE.store(true, Ordering::SeqCst);
        wake_pending_request();
        let control_result = control_thread.join().unwrap();
        FAIL_CHECKPOINT_WRITES.store(false, Ordering::SeqCst);
        control_result.unwrap();

        block_on(control.submit(Message::text(format!("after control {}", fixture.case)))).unwrap();

        let controlled_turn = drain_until_turn_ended(&mut events);
        assert_turn(&controlled_turn, TurnId(1), &fixture.case);
        assert!(
            controlled_turn
                .iter()
                .any(|event| matches!(event, SessionEvent::Error { .. })),
            "case {}: checkpoint degradation should be observable",
            fixture.case
        );
        let expected_first_output = if fixture.control == "interrupt" {
            vec![fixture.interrupted_output.clone()]
        } else {
            Vec::new()
        };
        assert_eq!(
            output_fragments(&controlled_turn),
            expected_first_output,
            "case {}",
            fixture.case
        );

        let next_turn = drain_until_turn_ended(&mut events);
        assert_turn(&next_turn, TurnId(2), &fixture.case);
        assert_eq!(
            output_fragments(&next_turn),
            vec![fixture.post_submit_output.clone()],
            "case {}",
            fixture.case
        );
    }
}

#[test]
fn close_reports_final_checkpoint_failure_after_closing_the_stream() {
    let _lock = CONTROL_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    FAIL_CHECKPOINT_WRITES.store(false, Ordering::SeqCst);
    MemFs::new();

    let root = mem_root("close-checkpoint-failure");
    let system = ControlSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    FAIL_CHECKPOINT_WRITES.store(true, Ordering::SeqCst);
    let result = block_on(control.close_session());
    FAIL_CHECKPOINT_WRITES.store(false, Ordering::SeqCst);

    assert_eq!(result, Err(SessionControlError::ClosePersistence));
    assert_eq!(
        block_on(events.next()),
        Some(SessionEvent::Closed),
        "close must complete even when its final checkpoint fails"
    );
}

#[derive(Default)]
struct ControlHttp;

impl ClawHttp for ControlHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        if !is_agent_iteration_request(&body) {
            return Box::pin(async { ok_response(assistant_text("[]")) });
        }
        if body.contains("after control") {
            let output = current_fixture().post_submit_output;
            return Box::pin(async move { ok_response(assistant_text(&output)) });
        }
        Box::pin(PendingResponse { cancel })
    }
}

struct PendingResponse<'a> {
    cancel: Cancel<'a>,
}

struct CheckpointFailFs;

impl ClawFs for CheckpointFailFs {
    type File = MemFile;

    fn open(path: &str) -> Result<Self::File, FsError> {
        MemFs::open(path)
    }

    fn create(path: &str) -> Result<Self::File, FsError> {
        if checkpoint_write_is_disabled(path) {
            return Err(FsError::io_message("checkpoint write disabled"));
        }
        MemFs::create(path)
    }

    fn open_append(path: &str) -> Result<Self::File, FsError> {
        MemFs::open_append(path)
    }

    fn rename(from: &str, to: &str) -> Result<(), FsError> {
        if checkpoint_write_is_disabled(to) {
            return Err(FsError::io_message("checkpoint write disabled"));
        }
        MemFs::rename(from, to)
    }

    fn create_dir_all(path: &str) -> Result<(), FsError> {
        MemFs::create_dir_all(path)
    }

    fn exists(path: &str) -> bool {
        MemFs::exists(path)
    }

    fn remove(path: &str) -> Result<(), FsError> {
        MemFs::remove(path)
    }

    fn list_dir(path: &str) -> Result<Vec<String>, FsError> {
        MemFs::list_dir(path)
    }
}

fn checkpoint_write_is_disabled(path: &str) -> bool {
    FAIL_CHECKPOINT_WRITES.load(Ordering::SeqCst) && path.contains("/checkpoint/")
}

impl Future for PendingResponse<'_> {
    type Output = Result<HttpResponse, HttpError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        REQUEST_POLLS.fetch_add(1, Ordering::SeqCst);
        if self.cancel.is_cancelled() {
            return Poll::Ready(Err(HttpError::Aborted));
        }
        if !ALLOW_RESPONSE.load(Ordering::SeqCst) {
            *REQUEST_WAKER
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner()) = Some(cx.waker().clone());
            return Poll::Pending;
        }
        Poll::Ready(ok_response(assistant_text(
            &current_fixture().interrupted_output,
        )))
    }
}

#[derive(Clone)]
struct Fixture {
    case: String,
    control: String,
    interrupted_output: String,
    post_submit_output: String,
}

impl Fixture {
    fn from_row(row: &BTreeMap<String, String>) -> Self {
        Self {
            case: field(row, "case").to_string(),
            control: field(row, "control").to_string(),
            interrupted_output: field(row, "interrupted_output").to_string(),
            post_submit_output: field(row, "post_submit_output").to_string(),
        }
    }
}

struct CaseState {
    fixture: Fixture,
}

fn install_case(fixture: Fixture) {
    REQUEST_POLLS.store(0, Ordering::SeqCst);
    CONTROL_SENT.store(false, Ordering::SeqCst);
    ALLOW_RESPONSE.store(false, Ordering::SeqCst);
    *REQUEST_WAKER
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner()) = None;
    *state() = Some(CaseState { fixture });
}

fn wake_pending_request() {
    if let Some(waker) = REQUEST_WAKER
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .take()
    {
        waker.wake();
    }
}

fn wait_for(flag: &AtomicUsize, case: &str, failure: &str) {
    for _ in 0..10_000 {
        if flag.load(Ordering::SeqCst) > 0 {
            return;
        }
        thread::yield_now();
    }
    panic!("case {case}: {failure}");
}

fn wait_for_bool(flag: &AtomicBool, case: &str, failure: &str) {
    for _ in 0..10_000 {
        if flag.load(Ordering::SeqCst) {
            return;
        }
        thread::yield_now();
    }
    panic!("case {case}: {failure}");
}

fn assert_turn(events: &[SessionEvent], turn: TurnId, case: &str) {
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted { turn }),
        "case {case}"
    );
    assert_eq!(
        events.last(),
        Some(&SessionEvent::TurnEnded { turn }),
        "case {case}"
    );
}

fn output_fragments(events: &[SessionEvent]) -> Vec<String> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::Output(StreamPart::Delta(text)) => Some(text.clone()),
            _ => None,
        })
        .collect()
}

fn current_fixture() -> Fixture {
    state()
        .as_ref()
        .expect("control test case installed")
        .fixture
        .clone()
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

fn ok_response(body: String) -> Result<HttpResponse, HttpError> {
    Ok(HttpResponse {
        status_code: HttpStatusCode::OK,
        body,
    })
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}

fn state() -> MutexGuard<'static, Option<CaseState>> {
    CASE_STATE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
