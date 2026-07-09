mod support;

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::BTreeMap;
use std::sync::{Mutex, MutexGuard};

use claw_agent::{AgentSystem, IterationId, SessionEvent, TurnCause, TurnId};
use claw_interface::{
    Cancel, ClawHttp, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type SubagentSystem = AgentSystem<MemFs, SubagentHttp, ImmediateTimer>;

static SUBAGENT_LOCK: Mutex<()> = Mutex::new(());
static SUBAGENT_STATE: Mutex<Option<SubagentCaseState>> = Mutex::new(None);

#[test]
fn subagent_lifecycle_csv_matrix_drives_background_results_and_graph_updates() {
    let _lock = SUBAGENT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/subagent_lifecycle_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        install_case(fixture.clone());

        let root = mem_root("subagent-lifecycle");
        let system =
            SubagentSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(&root))
                .unwrap();
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(format!("delegate {}", fixture.case))).unwrap();
        let first_turn = drain_until_turn_ended(&mut events);
        assert_turn(&first_turn, TurnId(1), TurnCause::UserSubmit, &fixture.case);
        assert_eq!(
            iteration_ids(&first_turn),
            vec![IterationId(0), IterationId(1)],
            "case {}",
            fixture.case
        );
        assert_eq!(
            tools_events(&first_turn),
            vec![vec!["spawn_subagent".to_string()]],
            "case {}",
            fixture.case
        );
        assert_eq!(
            output_fragments(&first_turn),
            vec![fixture.spawn_ack.clone()],
            "case {}",
            fixture.case
        );

        let background_turn = drain_until_turn_ended(&mut events);
        assert_turn(
            &background_turn,
            TurnId(2),
            TurnCause::BackgroundResult,
            &fixture.case,
        );
        assert_eq!(
            iteration_ids(&background_turn),
            vec![IterationId(0)],
            "case {}",
            fixture.case
        );
        assert_eq!(
            output_fragments(&background_turn),
            vec![fixture.background_output.clone()],
            "case {}",
            fixture.case
        );

        block_on(control.submit(format!("supervise {}", fixture.case))).unwrap();
        let supervision_turn = drain_until_turn_ended(&mut events);
        assert_turn(
            &supervision_turn,
            TurnId(3),
            TurnCause::UserSubmit,
            &fixture.case,
        );
        assert_eq!(
            iteration_ids(&supervision_turn),
            vec![IterationId(0), IterationId(1), IterationId(2)],
            "case {}",
            fixture.case
        );
        assert_eq!(
            tools_events(&supervision_turn),
            vec![
                vec![
                    "list_subagents".to_string(),
                    "watch_subagent".to_string(),
                    "delete_subagent".to_string(),
                ],
                vec!["list_subagents".to_string()],
            ],
            "case {}",
            fixture.case
        );
        assert_eq!(
            output_fragments(&supervision_turn),
            vec![fixture.supervision_output.clone()],
            "case {}",
            fixture.case
        );
        assert!(error_messages(&supervision_turn).is_empty());

        assert_request_history(&fixture);
    }
}

#[derive(Default)]
struct SubagentHttp;

impl ClawHttp for SubagentHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        let delay_once = should_delay_once(&body);
        Box::pin(SubagentResponseFuture {
            body,
            delay_once,
            yielded_pending: false,
        })
    }
}

struct SubagentResponseFuture {
    body: String,
    delay_once: bool,
    yielded_pending: bool,
}

impl Future for SubagentResponseFuture {
    type Output = Result<HttpResponse, claw_interface::HttpError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.delay_once && !self.yielded_pending {
            self.yielded_pending = true;
            cx.waker().wake_by_ref();
            return Poll::Pending;
        }

        Poll::Ready(Ok(HttpResponse {
            status_code: HttpStatusCode::OK,
            body: response_for_request(&self.body),
        }))
    }
}

#[derive(Clone)]
struct Fixture {
    case: String,
    termination: String,
    worker_output: String,
    spawn_ack: String,
    background_output: String,
    supervision_output: String,
    expected_watch_fragment: String,
    expected_delete_fragment: String,
    expected_after_delete_fragment: String,
}

impl Fixture {
    fn from_row(row: &BTreeMap<String, String>) -> Self {
        Self {
            case: field(row, "case").to_string(),
            termination: field(row, "termination").to_string(),
            worker_output: field(row, "worker_output").to_string(),
            spawn_ack: field(row, "spawn_ack").to_string(),
            background_output: field(row, "background_output").to_string(),
            supervision_output: field(row, "supervision_output").to_string(),
            expected_watch_fragment: field(row, "expected_watch_fragment").to_string(),
            expected_delete_fragment: field(row, "expected_delete_fragment").to_string(),
            expected_after_delete_fragment: field(row, "expected_after_delete_fragment")
                .to_string(),
        }
    }
}

struct SubagentCaseState {
    fixture: Fixture,
    root_requests: usize,
    worker_requests: usize,
    worker_delay_used: bool,
    child_id: Option<String>,
    requests: Vec<RecordedRequest>,
}

#[derive(Clone, Debug)]
struct RecordedRequest {
    kind: RequestKind,
    body: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum RequestKind {
    Extraction,
    Root,
    Worker,
    Other,
}

fn install_case(fixture: Fixture) {
    *state() = Some(SubagentCaseState {
        fixture,
        root_requests: 0,
        worker_requests: 0,
        worker_delay_used: false,
        child_id: None,
        requests: Vec::new(),
    });
}

fn should_delay_once(body: &str) -> bool {
    if classify_request(body) != RequestKind::Worker {
        return false;
    }

    let mut guard = state();
    let state = guard.as_mut().expect("subagent test case installed");
    if state.worker_delay_used {
        return false;
    }
    state.worker_delay_used = true;
    true
}

fn response_for_request(body: &str) -> String {
    let kind = classify_request(body);
    let mut guard = state();
    let state = guard.as_mut().expect("subagent test case installed");
    state.requests.push(RecordedRequest {
        kind,
        body: body.to_string(),
    });

    match kind {
        RequestKind::Extraction => assistant_text("[]"),
        RequestKind::Worker => {
            state.worker_requests += 1;
            assistant_text(&state.fixture.worker_output)
        }
        RequestKind::Root => root_response(state, body),
        RequestKind::Other => panic!("unexpected HTTP request body: {body}"),
    }
}

fn root_response(state: &mut SubagentCaseState, body: &str) -> String {
    let request_index = state.root_requests;
    state.root_requests += 1;

    match request_index {
        0 => assistant_tool_calls(vec![tool_call(
            "call_spawn",
            "spawn_subagent",
            json!({
                "kind": "worker",
                "name": "helper",
                "goal": format!("worker goal {}", state.fixture.case),
                "termination": state.fixture.termination,
            }),
        )]),
        1 => {
            state.child_id = extract_child_id(body);
            assistant_text(&state.fixture.spawn_ack)
        }
        2 => assistant_text(&state.fixture.background_output),
        3 => {
            let child_id = state.child_id.as_deref().expect("spawn response recorded");
            assistant_tool_calls(vec![
                tool_call("call_list_before_delete", "list_subagents", json!({})),
                tool_call(
                    "call_watch_before_delete",
                    "watch_subagent",
                    json!({ "agent": child_id }),
                ),
                tool_call(
                    "call_delete_subagent",
                    "delete_subagent",
                    json!({ "agent": child_id }),
                ),
            ])
        }
        4 => assistant_tool_calls(vec![tool_call(
            "call_list_after_delete",
            "list_subagents",
            json!({}),
        )]),
        5 => assistant_text(&state.fixture.supervision_output),
        other => panic!(
            "unexpected root request index {other} for {}",
            state.fixture.case
        ),
    }
}

fn assistant_tool_calls(calls: Vec<Value>) -> String {
    json!({
        "choices": [{
            "message": {
                "role": "assistant",
                "tool_calls": calls,
            }
        }]
    })
    .to_string()
}

fn tool_call(id: &str, name: &str, args: Value) -> Value {
    json!({
        "id": id,
        "type": "function",
        "function": {
            "name": name,
            "arguments": args.to_string(),
        },
    })
}

fn classify_request(body: &str) -> RequestKind {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return RequestKind::Other;
    };
    if value.get("response_format").is_some() {
        return RequestKind::Extraction;
    }
    let system = value["messages"]
        .as_array()
        .and_then(|messages| messages.first())
        .and_then(|message| message["content"].as_str())
        .unwrap_or_default();
    if value.get("tools").is_none() {
        return RequestKind::Extraction;
    }

    if system.contains("subagent spawned by the root agent") {
        RequestKind::Worker
    } else if system.contains("user-facing assistant") {
        RequestKind::Root
    } else {
        RequestKind::Other
    }
}

fn extract_child_id(body: &str) -> Option<String> {
    let value: Value = serde_json::from_str(body).ok()?;
    value["messages"]
        .as_array()?
        .iter()
        .filter(|message| message["role"].as_str() == Some("tool"))
        .filter_map(|message| message["content"].as_str())
        .flat_map(str::split_whitespace)
        .map(|word| word.trim_matches(|ch: char| !ch.is_ascii_alphanumeric() && ch != '-'))
        .find(|word| {
            word.strip_prefix("agent-").is_some_and(|digits| {
                !digits.is_empty() && digits.chars().all(|ch| ch.is_ascii_digit())
            })
        })
        .map(str::to_string)
}

fn assert_request_history(fixture: &Fixture) {
    let requests = recorded_requests();
    let root_requests = requests
        .iter()
        .filter(|request| request.kind == RequestKind::Root)
        .collect::<Vec<_>>();
    let worker_requests = requests
        .iter()
        .filter(|request| request.kind == RequestKind::Worker)
        .collect::<Vec<_>>();

    assert_eq!(
        root_requests.len(),
        6,
        "case {}: root request count in {requests:?}",
        fixture.case
    );
    assert_eq!(
        worker_requests.len(),
        1,
        "case {}: worker request count in {requests:?}",
        fixture.case
    );
    assert!(
        worker_requests[0]
            .body
            .contains(&format!("worker goal {}", fixture.case)),
        "case {}: worker did not receive delegated goal",
        fixture.case
    );

    let child_id = extract_child_id(&root_requests[1].body)
        .unwrap_or_else(|| panic!("case {}: child id missing", fixture.case));
    assert!(
        root_requests[2].body.contains(&format!(
            "[subagent {child_id} ok] {}",
            fixture.worker_output
        )),
        "case {}: background root request did not receive subagent result",
        fixture.case
    );

    let first_supervision_followup = &root_requests[4].body;
    assert_fragments(
        first_supervision_followup,
        &fixture.expected_watch_fragment,
        &fixture.case,
    );
    assert_fragments(
        first_supervision_followup,
        &fixture.expected_delete_fragment,
        &fixture.case,
    );

    assert_fragments(
        &root_requests[5].body,
        &fixture.expected_after_delete_fragment,
        &fixture.case,
    );
}

fn assert_fragments(body: &str, fragments: &str, case: &str) {
    let message_content = tool_message_content(body).join("\n");
    for fragment in fragments.split('|').filter(|fragment| !fragment.is_empty()) {
        assert!(
            body.contains(fragment) || message_content.contains(fragment),
            "case {case}: request body did not contain {fragment:?}: {body}"
        );
    }
}

fn tool_message_content(body: &str) -> Vec<String> {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return Vec::new();
    };
    value["messages"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|message| message["role"].as_str() == Some("tool"))
        .filter_map(|message| message["content"].as_str())
        .map(str::to_string)
        .collect()
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

fn iteration_ids(events: &[SessionEvent]) -> Vec<IterationId> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::IterationStarted { iteration } => Some(*iteration),
            _ => None,
        })
        .collect()
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

fn error_messages(events: &[SessionEvent]) -> Vec<String> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::Error { message } => Some(message.clone()),
            _ => None,
        })
        .collect()
}

fn recorded_requests() -> Vec<RecordedRequest> {
    state()
        .as_ref()
        .expect("subagent test case installed")
        .requests
        .clone()
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}

fn state() -> MutexGuard<'static, Option<SubagentCaseState>> {
    SUBAGENT_STATE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
