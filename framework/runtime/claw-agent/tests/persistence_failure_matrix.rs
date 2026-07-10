#![allow(clippy::unwrap_used)]

mod support;

use std::collections::{BTreeMap, VecDeque};
use std::sync::{Mutex, MutexGuard};

use claw_agent::{AgentSystem, SessionEvent};
use claw_interface::{
    Cancel, ClawFs, ClawHttp, DiskFs, HttpJsonRequest, HttpResponse, HttpResponseFuture,
    HttpStatusCode, ImmediateTimer, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{assistant_text, csv_dicts, drain_until_turn_ended, llm_config, persistence};
use tempdir::TempDir;

type PersistenceFailureSystem = AgentSystem<DiskFs, PersistenceFailureHttp, ImmediateTimer>;

static PERSISTENCE_FAILURE_LOCK: Mutex<()> = Mutex::new(());
static PERSISTENCE_FAILURE_REPLIES: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static PERSISTENCE_FAILURE_REQUESTS: Mutex<Vec<String>> = Mutex::new(Vec::new());

#[test]
fn persistence_failure_csv_matrix_reports_corrupt_disk_state_publicly() {
    let _lock = PERSISTENCE_FAILURE_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/persistence_failure_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        let temp = TempDir::new("claw-agent-persistence-failure").unwrap();
        let root = temp.path().to_string_lossy().into_owned();
        setup_disk_state(&root, &fixture.setup);

        match fixture.operation.as_str() {
            "startup" => assert_startup_error(&root, &fixture),
            "submit" => assert_submit_error(&root, &fixture),
            "tool" => assert_tool_error(&root, &fixture),
            other => panic!("case {}: unknown operation {other}", fixture.case),
        }
    }
}

#[derive(Default)]
struct PersistenceFailureHttp;

impl ClawHttp for PersistenceFailureHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        Box::pin(async move {
            let response = if is_agent_iteration_request(&body) {
                requests().push(body);
                replies()
                    .pop_front()
                    .expect("persistence failure script exhausted")
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

#[derive(Clone)]
struct Fixture {
    case: String,
    setup: String,
    operation: String,
    expected_error: String,
    final_output: String,
}

impl Fixture {
    fn from_row(row: &BTreeMap<String, String>) -> Self {
        Self {
            case: field(row, "case").to_string(),
            setup: field(row, "setup").to_string(),
            operation: field(row, "operation").to_string(),
            expected_error: field(row, "expected_error").to_string(),
            final_output: field(row, "final_output").to_string(),
        }
    }
}

fn setup_disk_state(root: &str, setup: &str) {
    match setup {
        "global_memory_journal_dir" => {
            DiskFs::create_dir_all(&format!("{root}/long_term/global/memory_records.jsonl"))
                .unwrap();
        }
        "root_transcript_log_dir" => {
            DiskFs::create_dir_all(&format!("{root}/transcript/1.jsonl")).unwrap();
        }
        "profile_user_invalid_utf8" => {
            DiskFs::create_dir_all(&format!("{root}/profile")).unwrap();
            DiskFs::write_atomic(&format!("{root}/profile/user.md"), &[0xff, 0xfe, 0xfd]).unwrap();
        }
        other => panic!("unknown persistence failure setup: {other}"),
    }
}

fn assert_startup_error(root: &str, fixture: &Fixture) {
    install_replies(Vec::new());
    let error = match PersistenceFailureSystem::new::<StdThread, TokioExecutor>(
        llm_config(),
        persistence(root),
    ) {
        Ok(_) => panic!("case {}: startup should fail", fixture.case),
        Err(error) => error.to_string(),
    };
    assert_contains(&error, &fixture.expected_error, &fixture.case);
}

fn assert_submit_error(root: &str, fixture: &Fixture) {
    install_replies(Vec::new());
    let system =
        PersistenceFailureSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root))
            .unwrap();
    let session = system.new_session();
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit(format!("trigger {}", fixture.case))).unwrap();
    let events = drain_until_turn_ended(&mut events);
    let errors = error_messages(&events).join("\n");
    assert_contains(&errors, &fixture.expected_error, &fixture.case);
}

fn assert_tool_error(root: &str, fixture: &Fixture) {
    install_replies(vec![
        assistant_tool_calls(vec![call(
            "call_profile_read",
            "profile.read",
            json!({ "document": "user_profile" }),
        )]),
        assistant_text(&fixture.final_output),
    ]);
    let system =
        PersistenceFailureSystem::new::<StdThread, TokioExecutor>(llm_config(), persistence(root))
            .unwrap();
    let session = system.new_session();
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit(format!("trigger {}", fixture.case))).unwrap();
    let events = drain_until_turn_ended(&mut events);

    assert!(
        events.iter().any(
            |event| matches!(event, SessionEvent::Output { text } if text == &fixture.final_output)
        ),
        "case {}: {events:?}",
        fixture.case
    );
    let requests = requests().clone();
    assert_eq!(requests.len(), 2, "case {}: {requests:?}", fixture.case);
    let tool_content = tool_message_content(&requests[1], &fixture.case).join("\n");
    assert_contains(&tool_content, &fixture.expected_error, &fixture.case);
}

#[derive(Clone)]
struct ToolCallSpec {
    id: &'static str,
    name: &'static str,
    args: Value,
}

fn call(id: &'static str, name: &'static str, args: Value) -> ToolCallSpec {
    ToolCallSpec { id, name, args }
}

fn assistant_tool_calls(calls: Vec<ToolCallSpec>) -> String {
    let tool_calls = calls
        .into_iter()
        .map(|call| {
            json!({
                "id": call.id,
                "type": "function",
                "function": {
                    "name": call.name,
                    "arguments": call.args.to_string(),
                },
            })
        })
        .collect::<Vec<_>>();
    json!({
        "choices": [{
            "message": {
                "role": "assistant",
                "tool_calls": tool_calls,
            }
        }]
    })
    .to_string()
}

fn install_replies(items: Vec<String>) {
    *replies() = items.into();
    requests().clear();
}

fn tool_message_content(body: &str, case: &str) -> Vec<String> {
    let value: Value = serde_json::from_str(body).unwrap();
    value["messages"]
        .as_array()
        .unwrap_or_else(|| panic!("case {case}: messages should be an array"))
        .iter()
        .filter(|message| message["role"].as_str() == Some("tool"))
        .map(|message| {
            message["content"]
                .as_str()
                .unwrap_or_else(|| panic!("case {case}: tool message missing content"))
                .to_string()
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

fn assert_contains(text: &str, fragment: &str, case: &str) {
    assert!(
        text.contains(fragment),
        "case {case}: expected {text:?} to contain {fragment:?}"
    );
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
        .trim()
}

fn replies() -> MutexGuard<'static, VecDeque<String>> {
    PERSISTENCE_FAILURE_REPLIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn requests() -> MutexGuard<'static, Vec<String>> {
    PERSISTENCE_FAILURE_REQUESTS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
