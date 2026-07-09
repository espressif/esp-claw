mod support;

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::{BTreeMap, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::thread;

use claw_agent::{AgentSystem, SessionEvent, TurnCause, TurnId};
use claw_interface::{
    Cancel, ClawHttp, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use claw_tool::{
    AsyncToolHandler, Tool, ToolFuture, ToolInvocation, ToolOutput, ToolResult, ToolSpec,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type AsyncToolSystem = AgentSystem<MemFs, AsyncToolHttp, ImmediateTimer>;

static ASYNC_TOOL_LOCK: Mutex<()> = Mutex::new(());
static ASYNC_TOOL_STATE: Mutex<Option<AsyncToolCaseState>> = Mutex::new(None);
static ASYNC_TOOL_POLLS: AtomicUsize = AtomicUsize::new(0);
static ASYNC_TOOL_COMPLETIONS: AtomicUsize = AtomicUsize::new(0);
static ALLOW_ASYNC_TOOL_COMPLETION: AtomicBool = AtomicBool::new(false);

#[test]
fn async_tool_control_csv_matrix_covers_cancel_and_interrupt_while_tool_is_pending() {
    let _lock = ASYNC_TOOL_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/async_tool_control_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        install_case(fixture.clone());

        let root = mem_root("async-tool-control");
        let system =
            AsyncToolSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(&root))
                .unwrap();
        system
            .tool_registry()
            .register(Tool::from_async(AsyncProbeTool))
            .unwrap();
        system.start_all().unwrap();

        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();
        block_on(control.submit(format!("run {}", fixture.case))).unwrap();
        wait_until_tool_is_pending(&fixture.case);

        match fixture.control.as_str() {
            "cancel" => block_on(control.cancel()).unwrap(),
            "interrupt" => block_on(control.interrupt()).unwrap(),
            other => panic!("case {}: unsupported control {other}", fixture.case),
        }
        ALLOW_ASYNC_TOOL_COMPLETION.store(true, Ordering::SeqCst);

        let first_turn = drain_until_turn_ended(&mut events);
        assert_turn(&first_turn, TurnId(1), TurnCause::UserSubmit, &fixture.case);
        assert_eq!(
            tools_events(&first_turn),
            vec![vec!["async_probe".to_string()]],
            "case {}",
            fixture.case
        );
        assert!(
            output_fragments(&first_turn).is_empty(),
            "case {}: controlled tool turn must not surface output: {first_turn:?}",
            fixture.case
        );
        assert_eq!(
            ASYNC_TOOL_COMPLETIONS.load(Ordering::SeqCst),
            1,
            "case {}",
            fixture.case
        );

        let next_turn = if fixture.control == "interrupt" {
            let background = drain_until_turn_ended(&mut events);
            assert_turn(
                &background,
                TurnId(2),
                TurnCause::BackgroundResult,
                &fixture.case,
            );
            assert_eq!(
                output_fragments(&background),
                vec![fixture.background_output.clone()],
                "case {}",
                fixture.case
            );
            TurnId(3)
        } else {
            TurnId(2)
        };

        block_on(control.submit(format!("after control {}", fixture.case))).unwrap();
        let after_control = drain_until_turn_ended(&mut events);
        assert_turn(
            &after_control,
            next_turn,
            TurnCause::UserSubmit,
            &fixture.case,
        );
        assert_eq!(
            output_fragments(&after_control),
            vec![fixture.post_submit_output.clone()],
            "case {}",
            fixture.case
        );

        assert_request_history(&fixture);
    }
}

#[derive(Default)]
struct AsyncToolHttp;

impl ClawHttp for AsyncToolHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        Box::pin(async move {
            let response = if is_agent_iteration_request(&body) {
                agent_response(&body)
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

struct AsyncProbeTool;

impl ToolSpec for AsyncProbeTool {
    fn name(&self) -> &str {
        "async_probe"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"async_probe","description":"async test tool","parameters":{"type":"object","properties":{"case":{"type":"string"}}}}}"#
    }

    fn usage(&self) -> Option<&str> {
        Some("Use async_probe only when the test fixture asks for it.")
    }
}

impl AsyncToolHandler for AsyncProbeTool {
    fn invoke<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a> {
        let arguments = call.arguments_json().to_string();
        Box::pin(AsyncProbeFuture { arguments })
    }
}

struct AsyncProbeFuture {
    arguments: String,
}

impl Future for AsyncProbeFuture {
    type Output = ToolResult<ToolOutput>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        ASYNC_TOOL_POLLS.fetch_add(1, Ordering::SeqCst);
        if !ALLOW_ASYNC_TOOL_COMPLETION.load(Ordering::SeqCst) {
            context.waker().wake_by_ref();
            return Poll::Pending;
        }

        ASYNC_TOOL_COMPLETIONS.fetch_add(1, Ordering::SeqCst);
        let output = current_fixture()
            .map(|fixture| fixture.tool_output)
            .unwrap_or_else(|| "async tool completed".to_string());
        Poll::Ready(Ok(ToolOutput {
            output: format!("{output}:{}", self.arguments),
            ok: true,
        }))
    }
}

#[derive(Clone)]
struct Fixture {
    case: String,
    control: String,
    background_output: String,
    post_submit_output: String,
    tool_output: String,
}

impl Fixture {
    fn from_row(row: &BTreeMap<String, String>) -> Self {
        Self {
            case: field(row, "case").to_string(),
            control: field(row, "control").to_string(),
            background_output: field(row, "background_output").to_string(),
            post_submit_output: field(row, "post_submit_output").to_string(),
            tool_output: field(row, "tool_output").to_string(),
        }
    }
}

struct AsyncToolCaseState {
    fixture: Fixture,
    root_requests: usize,
    request_bodies: VecDeque<String>,
}

fn install_case(fixture: Fixture) {
    ASYNC_TOOL_POLLS.store(0, Ordering::SeqCst);
    ASYNC_TOOL_COMPLETIONS.store(0, Ordering::SeqCst);
    ALLOW_ASYNC_TOOL_COMPLETION.store(false, Ordering::SeqCst);
    *state() = Some(AsyncToolCaseState {
        fixture,
        root_requests: 0,
        request_bodies: VecDeque::new(),
    });
}

fn agent_response(body: &str) -> String {
    let mut guard = state();
    let state = guard.as_mut().expect("async tool test case installed");
    state.request_bodies.push_back(body.to_string());

    if body.contains("after control") {
        return assistant_text(&state.fixture.post_submit_output);
    }
    if body.contains(&state.fixture.tool_output) {
        return assistant_text(&state.fixture.background_output);
    }

    let request_index = state.root_requests;
    state.root_requests = state.root_requests.saturating_add(1);
    match request_index {
        0 => assistant_tool_call(
            "async_probe",
            json!({ "case": state.fixture.case }).to_string(),
        ),
        other => panic!(
            "case {}: unexpected root request index {other}: {body}",
            state.fixture.case
        ),
    }
}

fn assistant_tool_call(name: &str, arguments_json: String) -> String {
    json!({
        "choices": [{
            "message": {
                "role": "assistant",
                "tool_calls": [{
                    "id": "call_async_probe",
                    "type": "function",
                    "function": {
                        "name": name,
                        "arguments": arguments_json,
                    },
                }]
            }
        }]
    })
    .to_string()
}

fn wait_until_tool_is_pending(case: &str) {
    for _ in 0..10_000 {
        if ASYNC_TOOL_POLLS.load(Ordering::SeqCst) > 0 {
            return;
        }
        thread::yield_now();
    }
    panic!("case {case}: async tool was not polled");
}

fn assert_request_history(fixture: &Fixture) {
    let bodies = recorded_request_bodies();
    assert!(
        bodies
            .iter()
            .any(|body| body.contains("\"name\":\"async_probe\"")),
        "case {}: initial request did not contain async_probe tool call context: {bodies:?}",
        fixture.case
    );
    if fixture.control == "interrupt" {
        assert!(
            bodies
                .iter()
                .any(|body| body.contains(&fixture.tool_output)),
            "case {}: interrupt should preserve async tool result for follow-up: {bodies:?}",
            fixture.case
        );
    }
    assert!(
        bodies.iter().any(|body| body.contains("after control")),
        "case {}: post-control submit request missing: {bodies:?}",
        fixture.case
    );
}

fn assert_turn(events: &[SessionEvent], turn: TurnId, cause: TurnCause, case: &str) {
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted { turn, cause }),
        "case {case}"
    );
    assert_eq!(
        events.last(),
        Some(&SessionEvent::TurnEnded { turn }),
        "case {case}"
    );
}

fn tools_events(events: &[SessionEvent]) -> Vec<Vec<String>> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::Tools { names } => Some(names.clone()),
            _ => None,
        })
        .collect()
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

fn recorded_request_bodies() -> Vec<String> {
    state()
        .as_ref()
        .expect("async tool test case installed")
        .request_bodies
        .iter()
        .cloned()
        .collect()
}

fn current_fixture() -> Option<Fixture> {
    state().as_ref().map(|state| state.fixture.clone())
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}

fn state() -> MutexGuard<'static, Option<AsyncToolCaseState>> {
    ASYNC_TOOL_STATE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
