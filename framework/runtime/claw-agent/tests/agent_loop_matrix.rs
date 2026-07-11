#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::collections::{BTreeMap, VecDeque};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};

use claw_agent::{AgentSystem, IterationId, SessionEvent, TurnCause, TurnId};
use claw_interface::{
    Cancel, ClawHttp, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use claw_tool::{
    SyncToolHandler, Tool, ToolError, ToolGroup, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolResult, ToolSpec,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type MatrixAgentSystem = AgentSystem<MemFs, Sse<AgentLoopHttp>, ImmediateTimer>;

static AGENT_LOOP_LOCK: Mutex<()> = Mutex::new(());
static AGENT_REPLIES: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static AGENT_REQUEST_BODIES: Mutex<Vec<String>> = Mutex::new(Vec::new());
static TOOL_INVOCATIONS: AtomicUsize = AtomicUsize::new(0);

#[test]
fn agent_loop_csv_tool_matrix_runs_tools_and_feeds_results_to_next_iteration() {
    let _lock = AGENT_LOOP_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/tool_loop_cases.csv")) {
        let case = field(&row, "case");
        let arguments = field(&row, "tool_arguments");
        let final_output = field(&row, "final_output");
        install_agent_replies(vec![
            assistant_tool_call("matrix_echo", arguments, Some(&format!("reasoning-{case}"))),
            assistant_text(final_output),
        ]);

        let root = mem_root("agent-loop-tools");
        let system = build_matrix_system(&root);
        apply_registry_ops(
            &system,
            field(&row, "registry_ops"),
            parse_tool_behavior(field(&row, "tool_behavior")),
        );
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(format!("run tool matrix {case}"))).unwrap();
        let events = drain_until_turn_ended(&mut events);

        assert_turn_bracket(&events, case);
        assert_eq!(
            iteration_ids(&events),
            vec![IterationId(0), IterationId(1)],
            "case {case}: expected tool iteration followed by final iteration"
        );
        assert_eq!(
            tools_events(&events),
            vec!["matrix_echo".to_string()],
            "case {case}"
        );
        assert!(
            reasoning_fragments(&events)
                .iter()
                .any(|text| text == &format!("reasoning-{case}")),
            "case {case}: reasoning event missing from first iteration: {events:?}"
        );
        assert_eq!(output_fragments(&events), vec![final_output.to_string()]);
        assert!(
            error_messages(&events).is_empty(),
            "case {case}: {events:?}"
        );
        assert_eq!(
            TOOL_INVOCATIONS.load(Ordering::SeqCst),
            parse_usize(&row, "expected_invocations"),
            "case {case}"
        );

        let bodies = agent_request_bodies().clone();
        assert_eq!(
            bodies.len(),
            2,
            "case {case}: expected one tool-call request and one follow-up request"
        );
        assert_agent_request_offered_expected_tool(&bodies[0], field(&row, "registry_ops"), case);
        assert_followup_received_tool_result(
            &bodies[1],
            arguments,
            field(&row, "expected_tool_error_contains"),
            case,
        );
    }
}

#[test]
fn agent_loop_csv_llm_response_matrix_reports_errors_and_bounds_reasoning() {
    let _lock = AGENT_LOOP_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/llm_response_cases.csv")) {
        let case = field(&row, "case");
        install_agent_replies(vec![llm_response_for_case(
            field(&row, "response_kind"),
            field(&row, "expected_output"),
            parse_usize(&row, "reasoning_bytes"),
        )]);

        let root = mem_root("agent-loop-llm");
        let system = build_matrix_system(&root);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(format!("run llm response matrix {case}"))).unwrap();
        let events = drain_until_turn_ended(&mut events);

        assert_turn_bracket(&events, case);
        assert_eq!(iteration_ids(&events), vec![IterationId(0)], "case {case}");
        assert_expected_output_and_error(
            &events,
            field(&row, "expected_output"),
            field(&row, "expected_error_contains"),
            case,
        );
        assert_reasoning_shape(
            &events,
            parse_usize(&row, "expected_reasoning_len"),
            field(&row, "expected_reasoning_suffix"),
            case,
        );
    }
}

#[derive(Default)]
struct AgentLoopHttp;

impl ClawHttp for AgentLoopHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            let body = if is_agent_iteration_request(request.body) {
                agent_request_bodies().push(request.body.to_owned());
                agent_replies()
                    .pop_front()
                    .expect("agent loop request consumed more replies than scripted")
            } else {
                assistant_text("[]")
            };
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body,
            })
        })
    }
}

#[derive(Clone, Copy)]
enum MatrixToolBehavior {
    Echo,
    Reject,
    OkFalse,
}

struct MatrixTool {
    behavior: MatrixToolBehavior,
}

impl ToolSpec for MatrixTool {
    fn name(&self) -> &str {
        "matrix_echo"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"matrix_echo","description":"test echo tool","parameters":{"type":"object","properties":{"value":{"type":"string"}}}}}"#
    }

    fn usage(&self) -> Option<&str> {
        Some("Use matrix_echo only when the test fixture asks for it.")
    }
}

impl SyncToolHandler for MatrixTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        TOOL_INVOCATIONS.fetch_add(1, Ordering::SeqCst);
        match self.behavior {
            MatrixToolBehavior::Echo => Ok(ToolOutput {
                output: format!("tool-output:{}", call.arguments_json()),
                ok: true,
            }),
            MatrixToolBehavior::Reject => Err(ToolInvokeError::new(ToolError::InvokeRejected(
                "denied-by-test".to_string(),
            ))),
            MatrixToolBehavior::OkFalse => Ok(ToolOutput {
                output: "soft-failed".to_string(),
                ok: false,
            }),
        }
    }
}

fn build_matrix_system(root: &str) -> MatrixAgentSystem {
    MatrixAgentSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root)).unwrap()
}

fn apply_registry_ops(system: &MatrixAgentSystem, operations: &str, behavior: MatrixToolBehavior) {
    for operation in operations.split('|') {
        match operation {
            "register" => system
                .tool_registry()
                .register_group(ToolGroup::new(
                    "matrix",
                    true,
                    [Tool::from_sync(MatrixTool { behavior })],
                ))
                .unwrap(),
            "start" => system.start_all().unwrap(),
            "stop" => system.stop_all().unwrap(),
            "enable" => system.tool_registry().enable("matrix_echo").unwrap(),
            "disable" => system.tool_registry().disable("matrix_echo").unwrap(),
            other => panic!("unknown registry op in fixture: {other}"),
        }
    }
}

fn assistant_tool_call(name: &str, arguments_json: &str, reasoning: Option<&str>) -> String {
    let mut message = json!({
        "role": "assistant",
        "tool_calls": [{
            "id": "call_matrix_1",
            "type": "function",
            "function": {
                "name": name,
                "arguments": arguments_json,
            },
        }],
    });
    if let Some(reasoning) = reasoning {
        message["reasoning_content"] = Value::String(reasoning.to_string());
    }
    json!({ "choices": [{ "message": message }] }).to_string()
}

fn llm_response_for_case(kind: &str, output: &str, reasoning_bytes: usize) -> String {
    match kind {
        "plain" => assistant_plain_response(output, reasoning_bytes),
        "missing_message" => json!({ "choices": [{}] }).to_string(),
        "non_assistant" => {
            json!({ "choices": [{ "message": { "role": "user", "content": output } }] }).to_string()
        }
        "empty_message" => {
            json!({ "choices": [{ "message": { "role": "assistant", "content": "" } }] })
                .to_string()
        }
        "invalid_json" => "not-json".to_string(),
        "malformed_tool_call" => json!({
            "choices": [{
                "message": {
                    "role": "assistant",
                    "tool_calls": [{
                        "id": "call_bad",
                        "type": "function",
                        "function": { "name": "matrix_echo" }
                    }]
                }
            }]
        })
        .to_string(),
        other => panic!("unknown response kind in fixture: {other}"),
    }
}

fn assistant_plain_response(output: &str, reasoning_bytes: usize) -> String {
    let mut message = json!({ "role": "assistant", "content": output });
    if reasoning_bytes > 0 {
        message["reasoning_content"] = Value::String("r".repeat(reasoning_bytes));
    }
    json!({ "choices": [{ "message": message }] }).to_string()
}

fn install_agent_replies(replies: Vec<String>) {
    *agent_replies() = replies.into();
    agent_request_bodies().clear();
    TOOL_INVOCATIONS.store(0, Ordering::SeqCst);
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

fn assert_agent_request_offered_expected_tool(body: &str, operations: &str, case: &str) {
    let value: Value = serde_json::from_str(body).unwrap();
    let offered_tool_names = value["tools"]
        .as_array()
        .unwrap_or_else(|| panic!("case {case}: tools should be an array in {body}"))
        .iter()
        .filter_map(|tool| {
            tool.get("function")
                .and_then(|function| function.get("name"))
                .and_then(Value::as_str)
        })
        .collect::<Vec<_>>();
    let should_offer_matrix_tool = registry_ops_leave_tool_started_and_enabled(operations);
    assert_eq!(
        offered_tool_names.contains(&"matrix_echo"),
        should_offer_matrix_tool,
        "case {case}: offered tools were {offered_tool_names:?}"
    );
}

fn registry_ops_leave_tool_started_and_enabled(operations: &str) -> bool {
    let mut registered = false;
    let mut enabled = false;
    let mut started = false;
    for operation in operations.split('|') {
        match operation {
            "register" => {
                registered = true;
                enabled = true;
            }
            "enable" => enabled = true,
            "disable" => enabled = false,
            "start" => started = true,
            "stop" => started = false,
            other => panic!("unknown registry op in fixture: {other}"),
        }
    }
    registered && enabled && started
}

fn assert_followup_received_tool_result(
    body: &str,
    original_arguments: &str,
    expected_error: &str,
    case: &str,
) {
    let value: Value = serde_json::from_str(body).unwrap();
    let messages = value["messages"]
        .as_array()
        .unwrap_or_else(|| panic!("case {case}: messages should be an array"));
    let tool_message = messages
        .iter()
        .find(|message| message["role"].as_str() == Some("tool"))
        .unwrap_or_else(|| panic!("case {case}: follow-up request missing tool message"));
    let content = tool_message["content"]
        .as_str()
        .unwrap_or_else(|| panic!("case {case}: tool content should be a string"));

    if expected_error.is_empty() {
        assert_eq!(
            tool_message["is_error"].as_bool(),
            Some(false),
            "case {case}: {tool_message:?}"
        );
        assert!(
            content.contains(original_arguments),
            "case {case}: {content:?} should include {original_arguments:?}"
        );
    } else {
        assert_eq!(
            tool_message["is_error"].as_bool(),
            Some(true),
            "case {case}: {tool_message:?}"
        );
        assert!(
            content.contains(expected_error),
            "case {case}: {content:?} should include {expected_error:?}"
        );
    }
}

fn assert_expected_output_and_error(
    events: &[SessionEvent],
    expected_output: &str,
    expected_error: &str,
    case: &str,
) {
    if expected_output.is_empty() && expected_error.is_empty() {
        assert!(
            output_fragments(events).is_empty(),
            "case {case}: {events:?}"
        );
    } else if !expected_output.is_empty() {
        assert_eq!(
            output_fragments(events),
            vec![expected_output.to_string()],
            "case {case}"
        );
    }

    if expected_error.is_empty() {
        assert!(error_messages(events).is_empty(), "case {case}: {events:?}");
    } else {
        let errors = error_messages(events);
        let failure_texts = output_fragments(events)
            .into_iter()
            .chain(errors)
            .collect::<Vec<_>>();
        assert!(
            failure_texts
                .iter()
                .any(|message| message.contains(expected_error)),
            "case {case}: {failure_texts:?} should contain {expected_error:?}"
        );
    }
}

fn assert_reasoning_shape(
    events: &[SessionEvent],
    expected_len: usize,
    expected_suffix: &str,
    case: &str,
) {
    let reasonings = reasoning_fragments(events);
    if expected_len == 0 {
        assert!(reasonings.is_empty(), "case {case}: {reasonings:?}");
        return;
    }

    assert_eq!(reasonings.len(), 1, "case {case}: {reasonings:?}");
    assert_eq!(reasonings[0].len(), expected_len, "case {case}");
    if !expected_suffix.is_empty() {
        assert!(
            reasonings[0].ends_with(expected_suffix),
            "case {case}: {:?}",
            reasonings[0]
        );
    }
}

fn assert_turn_bracket(events: &[SessionEvent], case: &str) {
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted {
            turn: TurnId(1),
            cause: TurnCause::UserSubmit,
        }),
        "case {case}"
    );
    assert_eq!(
        events.last(),
        Some(&SessionEvent::TurnEnded { turn: TurnId(1) }),
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

fn reasoning_fragments(events: &[SessionEvent]) -> Vec<String> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::Reasoning { text } => Some(text.clone()),
            _ => None,
        })
        .collect()
}

fn tools_events(events: &[SessionEvent]) -> Vec<String> {
    events
        .iter()
        .filter_map(|event| match event {
            SessionEvent::ToolCall { name } => Some(name.clone()),
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

fn parse_tool_behavior(value: &str) -> MatrixToolBehavior {
    match value {
        "echo" => MatrixToolBehavior::Echo,
        "reject" => MatrixToolBehavior::Reject,
        "ok_false" => MatrixToolBehavior::OkFalse,
        other => panic!("invalid tool behavior in fixture: {other}"),
    }
}

fn parse_usize(row: &BTreeMap<String, String>, field_name: &str) -> usize {
    field(row, field_name).parse::<usize>().unwrap()
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}

fn agent_replies() -> MutexGuard<'static, VecDeque<String>> {
    AGENT_REPLIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn agent_request_bodies() -> MutexGuard<'static, Vec<String>> {
    AGENT_REQUEST_BODIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
