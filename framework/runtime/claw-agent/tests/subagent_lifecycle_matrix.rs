#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use core::future::Future;
use core::pin::Pin;
use core::task::{Context, Poll};
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};

use claw_agent::{
    AgentId, AgentSystem, IterationId, Message, SessionEvent, StreamPart, TurnId, TurnOrigin,
};
use claw_interface::{
    Cancel, ClawHttp, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type SubagentSystem = AgentSystem<MemFs, Sse<SubagentHttp>, ImmediateTimer>;

static SUBAGENT_LOCK: Mutex<()> = Mutex::new(());
static SUBAGENT_STATE: Mutex<Option<SubagentCaseState>> = Mutex::new(None);
static CONTROL_WORKER_POLLS: AtomicUsize = AtomicUsize::new(0);
static RELEASE_HELD_WORKER: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

#[test]
fn subagent_lifecycle_csv_matrix_drives_background_results_and_graph_updates() {
    let _lock = SUBAGENT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/subagent_lifecycle_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        install_case(fixture.clone(), false);

        let root = mem_root("subagent-lifecycle");
        let system = SubagentSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
        system
            .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
            .unwrap();
        let session = system.new_session(claw_agent::SessionPersistence::Persistent);
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(Message::text(format!("delegate {}", fixture.case)))).unwrap();
        let delegated_turn = drain_until_turn_ended(&mut events);
        assert_turn(&delegated_turn, TurnId(1), TurnOrigin::User, &fixture.case);
        assert_eq!(
            iteration_ids(&delegated_turn),
            vec![IterationId(0), IterationId(1), IterationId(0)],
            "case {}",
            fixture.case
        );
        assert_eq!(
            tools_events(&delegated_turn),
            vec!["subagent_spawn".to_string()],
            "case {}",
            fixture.case
        );
        assert_eq!(
            output_fragments(&delegated_turn),
            vec![fixture.spawn_ack.clone(), fixture.background_output.clone()],
            "case {}",
            fixture.case
        );

        block_on(control.submit(Message::text(format!("supervise {}", fixture.case)))).unwrap();
        let supervision_turn = drain_until_turn_ended(&mut events);
        assert_turn(
            &supervision_turn,
            TurnId(2),
            TurnOrigin::User,
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
                "subagent_list".to_string(),
                "subagent_watch".to_string(),
                "subagent_delete".to_string(),
                "subagent_list".to_string(),
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

#[test]
fn foreground_spawn_returns_the_child_result_to_the_same_tool_call_and_turn() {
    let _lock = SUBAGENT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let fixture = csv_dicts(include_str!("fixtures/subagent_lifecycle_cases.csv"))
        .into_iter()
        .map(|row| Fixture::from_row(&row))
        .next()
        .expect("subagent fixture");
    install_case(fixture.clone(), true);

    let root = mem_root("subagent-foreground");
    let system = SubagentSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(control.submit(Message::text("delegate in foreground"))).unwrap();
    let turn = drain_until_turn_ended(&mut events);
    assert_turn(&turn, TurnId(1), TurnOrigin::User, &fixture.case);
    assert_eq!(iteration_ids(&turn), vec![IterationId(0), IterationId(1)]);
    assert_eq!(tools_events(&turn), vec!["subagent_spawn".to_string()]);
    assert_eq!(output_fragments(&turn), vec![fixture.spawn_ack.clone()]);

    let requests = recorded_requests();
    let root_requests = requests
        .iter()
        .filter(|request| request.kind == RequestKind::Root)
        .collect::<Vec<_>>();
    assert_eq!(root_requests.len(), 2);
    assert_eq!(worker_request_count(), 1);
    let child = extract_child_id(&root_requests[1].body).expect("foreground result has child id");
    assert!(tool_message_content(&root_requests[1].body)
        .join("\n")
        .contains(&format!(
            "[subagent] id: {child}, result: true, message: {}",
            fixture.worker_output
        )));
    let request: Value = serde_json::from_str(&root_requests[1].body).expect("valid request body");
    assert!(!request["messages"]
        .as_array()
        .expect("request messages")
        .iter()
        .any(|message| {
            message["role"].as_str() == Some("user")
                && message["content"]
                    .as_str()
                    .is_some_and(|content| content.starts_with("[subagent]"))
        }));
}

#[test]
fn a_user_turn_can_run_while_a_background_subagent_is_still_working() {
    let _lock = SUBAGENT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    install_background_concurrency_case();

    let root = mem_root("subagent-background-concurrency");
    let system = SubagentSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(control.submit(Message::text("start background work"))).unwrap();
    let delegated = drain_until_turn_ended(&mut events);
    assert_turn(&delegated, TurnId(1), TurnOrigin::User, "background");
    wait_until_control_worker_is_pending("background");

    block_on(control.submit(Message::text("answer while it runs"))).unwrap();
    let concurrent = drain_until_turn_ended(&mut events);
    assert_turn(&concurrent, TurnId(2), TurnOrigin::User, "concurrent user");
    assert_eq!(
        output_fragments(&concurrent),
        vec!["root stayed responsive"]
    );

    RELEASE_HELD_WORKER.store(true, Ordering::SeqCst);
    let child = recorded_child_id();
    let delivered = drain_until_turn_ended(&mut events);
    assert_turn(
        &delivered,
        TurnId(3),
        TurnOrigin::Subagent { agent: child },
        "background result",
    );
    assert_eq!(output_fragments(&delivered), vec!["background delivered"]);
}

#[test]
fn cancelled_foreground_spawn_deletes_its_subagent() {
    let _lock = SUBAGENT_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    let control_name = "cancel";
    install_control_case(control_name);
    let root = mem_root("subagent-turn-control");
    let system = SubagentSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(control.submit(Message::text(format!("delegate then {control_name}")))).unwrap();
    wait_until_control_worker_is_pending(control_name);
    block_on(control.cancel()).unwrap();
    block_on(control.submit(Message::text(format!("inspect after {control_name}")))).unwrap();

    let controlled_turn = drain_until_turn_ended(&mut events);
    assert_turn(&controlled_turn, TurnId(1), TurnOrigin::User, control_name);

    let inspection_turn = drain_until_turn_ended(&mut events);
    assert_turn(&inspection_turn, TurnId(2), TurnOrigin::User, control_name);
    assert_eq!(
        output_fragments(&inspection_turn),
        vec![format!("{control_name} graph inspected")],
    );

    let list_result = control_list_result();
    assert!(
        list_result.contains(r#""subagents":[]"#),
        "cancel should delete every spawned subagent: {list_result}"
    );
}

#[derive(Default)]
struct SubagentHttp;

impl ClawHttp for SubagentHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        if should_hold_worker(&body) {
            return Box::pin(async move {
                loop {
                    CONTROL_WORKER_POLLS.fetch_add(1, Ordering::SeqCst);
                    if cancel.is_cancelled() {
                        return Err(claw_interface::HttpError::Aborted);
                    }
                    if RELEASE_HELD_WORKER.load(Ordering::SeqCst) {
                        return Ok(HttpResponse {
                            status_code: HttpStatusCode::OK,
                            body: response_for_request(&body),
                        });
                    }
                    YieldOnce(false).await;
                }
            });
        }
        let delay_once = should_delay_once(&body);
        let wait_for_worker = should_wait_for_worker(&body);
        Box::pin(SubagentResponseFuture {
            body,
            delay_once,
            yielded_pending: false,
            wait_for_worker,
            yielded_after_worker: false,
        })
    }
}

struct YieldOnce(bool);

impl Future for YieldOnce {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.0 {
            Poll::Ready(())
        } else {
            self.0 = true;
            cx.waker().wake_by_ref();
            Poll::Pending
        }
    }
}

struct SubagentResponseFuture {
    body: String,
    delay_once: bool,
    yielded_pending: bool,
    wait_for_worker: bool,
    yielded_after_worker: bool,
}

impl Future for SubagentResponseFuture {
    type Output = Result<HttpResponse, claw_interface::HttpError>;

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        if self.wait_for_worker && worker_request_count() == 0 {
            cx.waker().wake_by_ref();
            return Poll::Pending;
        }
        if self.wait_for_worker && !self.yielded_after_worker {
            self.yielded_after_worker = true;
            cx.waker().wake_by_ref();
            return Poll::Pending;
        }
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
    control: Option<String>,
    foreground: bool,
    background_concurrency: bool,
    hold_worker: bool,
    wait_root_for_worker: bool,
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

fn install_case(fixture: Fixture, foreground: bool) {
    *state() = Some(SubagentCaseState {
        fixture,
        control: None,
        foreground,
        background_concurrency: false,
        hold_worker: false,
        wait_root_for_worker: !foreground,
        root_requests: 0,
        worker_requests: 0,
        worker_delay_used: false,
        child_id: None,
        requests: Vec::new(),
    });
}

fn install_control_case(control: &str) {
    CONTROL_WORKER_POLLS.store(0, Ordering::SeqCst);
    RELEASE_HELD_WORKER.store(false, Ordering::SeqCst);
    *state() = Some(SubagentCaseState {
        fixture: Fixture {
            case: control.to_string(),
            worker_output: "must not finish".to_string(),
            spawn_ack: "root delegated".to_string(),
            background_output: String::new(),
            supervision_output: String::new(),
            expected_watch_fragment: String::new(),
            expected_delete_fragment: String::new(),
            expected_after_delete_fragment: String::new(),
        },
        control: Some(control.to_string()),
        foreground: true,
        background_concurrency: false,
        hold_worker: true,
        wait_root_for_worker: false,
        root_requests: 0,
        worker_requests: 0,
        worker_delay_used: false,
        child_id: None,
        requests: Vec::new(),
    });
}

fn install_background_concurrency_case() {
    CONTROL_WORKER_POLLS.store(0, Ordering::SeqCst);
    RELEASE_HELD_WORKER.store(false, Ordering::SeqCst);
    *state() = Some(SubagentCaseState {
        fixture: Fixture {
            case: "background-concurrency".to_owned(),
            worker_output: "held worker result".to_owned(),
            spawn_ack: "background requested".to_owned(),
            background_output: "background delivered".to_owned(),
            supervision_output: String::new(),
            expected_watch_fragment: String::new(),
            expected_delete_fragment: String::new(),
            expected_after_delete_fragment: String::new(),
        },
        control: None,
        foreground: false,
        background_concurrency: true,
        hold_worker: true,
        wait_root_for_worker: false,
        root_requests: 0,
        worker_requests: 0,
        worker_delay_used: false,
        child_id: None,
        requests: Vec::new(),
    });
}

fn should_hold_worker(body: &str) -> bool {
    classify_request(body) == RequestKind::Worker
        && state().as_ref().is_some_and(|state| state.hold_worker)
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

fn should_wait_for_worker(body: &str) -> bool {
    classify_request(body) == RequestKind::Root
        && state()
            .as_ref()
            .is_some_and(|state| state.wait_root_for_worker && state.root_requests == 1)
}

fn worker_request_count() -> usize {
    state().as_ref().map_or(0, |state| state.worker_requests)
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

    if let Some(control) = state.control.clone() {
        return control_root_response(state, body, request_index, &control);
    }
    if state.background_concurrency {
        return background_concurrency_root_response(state, body, request_index);
    }

    match request_index {
        0 => assistant_tool_calls(vec![tool_call(
            "call_spawn",
            "subagent_spawn",
            json!({
                "kind": "worker",
                "name": "helper",
                "goal": format!("worker goal {}", state.fixture.case),
                "foreground": state.foreground,
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
                tool_call("call_list_before_delete", "subagent_list", json!({})),
                tool_call(
                    "call_watch_before_delete",
                    "subagent_watch",
                    json!({ "agent": child_id }),
                ),
                tool_call(
                    "call_subagent_delete",
                    "subagent_delete",
                    json!({ "agent": child_id }),
                ),
            ])
        }
        4 => assistant_tool_calls(vec![tool_call(
            "call_list_after_delete",
            "subagent_list",
            json!({}),
        )]),
        5 => assistant_text(&state.fixture.supervision_output),
        other => panic!(
            "unexpected root request index {other} for {}",
            state.fixture.case
        ),
    }
}

fn background_concurrency_root_response(
    state: &mut SubagentCaseState,
    body: &str,
    request_index: usize,
) -> String {
    match request_index {
        0 => assistant_tool_calls(vec![tool_call(
            "call_spawn",
            "subagent_spawn",
            json!({
                "kind": "worker",
                "name": "helper",
                "goal": "held background work",
                "foreground": false,
            }),
        )]),
        1 => {
            state.child_id = extract_child_id(body);
            assistant_text("background requested")
        }
        2 => assistant_text("root stayed responsive"),
        3 => assistant_text("background delivered"),
        other => panic!("unexpected background concurrency request index {other}"),
    }
}

fn control_root_response(
    _state: &mut SubagentCaseState,
    _body: &str,
    request_index: usize,
    control: &str,
) -> String {
    match request_index {
        0 => assistant_tool_calls(vec![tool_call(
            "call_spawn",
            "subagent_spawn",
            json!({
                "kind": "worker",
                "name": "helper",
                "goal": format!("worker held for {control}"),
                "foreground": true,
            }),
        )]),
        1 => assistant_tool_calls(vec![tool_call(
            "call_list_after_control",
            "subagent_list",
            json!({}),
        )]),
        2 => assistant_text(&format!("{control} graph inspected")),
        other => panic!("unexpected control root request index {other} for {control}"),
    }
}

fn wait_until_control_worker_is_pending(control: &str) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(2);
    while std::time::Instant::now() < deadline {
        if CONTROL_WORKER_POLLS.load(Ordering::SeqCst) > 0 {
            return;
        }
        std::thread::yield_now();
    }
    panic!("{control}: worker did not enter its pending task");
}

fn control_list_result() -> String {
    recorded_requests()
        .into_iter()
        .rev()
        .find(|request| request.kind == RequestKind::Root)
        .map(|request| tool_message_content(&request.body).join("\n"))
        .unwrap_or_default()
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
            "[subagent] id: {child_id}, result: true, message: {}",
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

fn recorded_child_id() -> AgentId {
    let child = state()
        .as_ref()
        .and_then(|state| state.child_id.clone())
        .expect("spawn response recorded child id");
    AgentId::from_wire(&child).expect("valid recorded child id")
}

fn assert_turn(events: &[SessionEvent], turn: TurnId, origin: TurnOrigin, case: &str) {
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted { turn, origin }),
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

fn tools_events(events: &[SessionEvent]) -> Vec<String> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::ToolCalls(StreamPart::Delta(call)) => Some(call.name.clone()),
            _ => None,
        })
        .collect()
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
