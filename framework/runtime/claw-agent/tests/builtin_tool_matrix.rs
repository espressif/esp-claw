#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::collections::{BTreeMap, VecDeque};
use std::sync::{Mutex, MutexGuard};

use claw_agent::{AgentSystem, IterationId, Message, SessionEvent, StreamPart, TurnId};
use claw_interface::{
    Cancel, ClawHttp, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{
    assistant_text, csv_dicts, drain_until_turn_ended, llm_config, mem_root, persistence,
};

type BuiltinToolSystem = AgentSystem<MemFs, Sse<BuiltinToolHttp>, ImmediateTimer>;

static BUILTIN_TOOL_LOCK: Mutex<()> = Mutex::new(());
static BUILTIN_REPLIES: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static BUILTIN_REQUEST_BODIES: Mutex<Vec<String>> = Mutex::new(Vec::new());

#[test]
fn builtin_tools_csv_matrix_feeds_profile_memory_and_subagent_results_back_to_llm() {
    let _lock = BUILTIN_TOOL_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/builtin_tool_cases.csv")) {
        let case = field(&row, "case");
        let sequence = field(&row, "sequence");
        let final_output = field(&row, "final_output");
        install_builtin_replies(vec![
            assistant_tool_calls(tool_calls_for_sequence(sequence)),
            assistant_text(final_output),
        ]);

        let root = mem_root("builtin-tool-matrix");
        MemFs::new();
        let system =
            BuiltinToolSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
        system
            .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
            .unwrap();
        let session = system.new_session(claw_agent::SessionPersistence::Persistent);
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(Message::text(format!("run builtin tool matrix {case}")))).unwrap();
        let events = drain_until_turn_ended(&mut events);

        assert_turn_bracket(&events, case);
        assert_eq!(
            iteration_ids(&events),
            vec![IterationId(0), IterationId(1)],
            "case {case}"
        );
        assert_eq!(
            tools_events(&events).len(),
            parse_usize(&row, "expected_tool_count"),
            "case {case}"
        );
        assert_eq!(output_fragments(&events), vec![final_output.to_string()]);
        assert!(
            error_messages(&events).is_empty(),
            "case {case}: {events:?}"
        );

        let bodies = builtin_request_bodies().clone();
        assert_eq!(bodies.len(), 2, "case {case}: {bodies:?}");
        assert_request_offered_builtin_tools(&bodies[0], sequence, case);
        let tool_messages = tool_messages_from_followup(&bodies[1], case);
        assert_fragments_in_tool_messages(
            &tool_messages,
            field(&row, "expected_ok_fragments"),
            Some(false),
            case,
        );
        assert_fragments_in_tool_messages(
            &tool_messages,
            field(&row, "expected_error_fragments"),
            Some(true),
            case,
        );
    }
}

#[derive(Default)]
struct BuiltinToolHttp;

impl ClawHttp for BuiltinToolHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            let body = if is_agent_iteration_request(request.body) {
                builtin_request_bodies().push(request.body.to_owned());
                builtin_replies()
                    .pop_front()
                    .expect("builtin tool request consumed more replies than scripted")
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

#[derive(Clone)]
struct ToolCallSpec {
    name: &'static str,
    args: Value,
}

fn tool_calls_for_sequence(sequence: &str) -> Vec<ToolCallSpec> {
    match sequence {
        "profile_replace_read" => vec![
            call(
                "profile_replace",
                json!({
                    "document": "user_profile",
                    "content": "Uses terse responses",
                }),
            ),
            call("profile_read", json!({ "document": "user_profile" })),
        ],
        "profile_clear_read" => vec![
            call(
                "profile_replace",
                json!({ "document": "soul", "content": "temporary soul" }),
            ),
            call("profile_clear", json!({ "document": "soul" })),
            call("profile_read", json!({ "document": "soul" })),
        ],
        "profile_invalid_document" => {
            vec![call("profile_read", json!({ "document": "unknown_doc" }))]
        }
        "memory_agent_store_recall" => vec![
            call(
                "memory_store",
                json!({
                    "content": "Task note survives recall",
                    "tags": ["task"],
                    "keywords": ["survives"],
                }),
            ),
            call(
                "memory_recall",
                json!({ "labels": ["task"], "query": "survives", "limit": 5 }),
            ),
        ],
        "memory_duplicate_store" => vec![
            call(
                "memory_store",
                json!({ "content": "Duplicate durable note", "tags": ["task"] }),
            ),
            call(
                "memory_store",
                json!({ "content": " duplicate   durable NOTE ", "tags": ["task"] }),
            ),
        ],
        "memory_global_update_list_forget" => vec![
            call(
                "memory_store",
                json!({
                    "content": "Initial global fact",
                    "tags": ["fact"],
                    "keywords": ["global"],
                }),
            ),
            call(
                "memory_update",
                json!({
                    "id": "g-0",
                    "content": "Updated global fact",
                    "tags": ["fact"],
                    "keywords": ["global", "updated"],
                }),
            ),
            call("memory_list", json!({ "limit": 5 })),
            call("memory_forget", json!({ "id": "g-0" })),
            call("memory_list", json!({ "limit": 5 })),
        ],
        "builtin_subagent_validation" => vec![
            call("subagent_list_spawnable", json!({})),
            call("subagent_list", json!({})),
            call("subagent_watch", json!({ "agent": "agent-999" })),
            call("subagent_delete", json!({ "agent": "agent-999" })),
            call(
                "subagent_followup",
                json!({ "agent": "agent-999", "message": "retask" }),
            ),
            call(
                "subagent_spawn",
                json!({
                    "kind": "ghost",
                    "name": "bad",
                    "goal": "do impossible work",
                    "termination": "manual",
                }),
            ),
        ],
        other => panic!("unknown builtin tool sequence: {other}"),
    }
}

fn call(name: &'static str, args: Value) -> ToolCallSpec {
    ToolCallSpec { name, args }
}

fn assistant_tool_calls(calls: Vec<ToolCallSpec>) -> String {
    let tool_calls = calls
        .into_iter()
        .enumerate()
        .map(|(index, call)| {
            json!({
                "id": format!("call_builtin_{index}"),
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

fn install_builtin_replies(replies: Vec<String>) {
    *builtin_replies() = replies.into();
    builtin_request_bodies().clear();
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

fn assert_request_offered_builtin_tools(body: &str, sequence: &str, case: &str) {
    let value: Value = serde_json::from_str(body).unwrap();
    let offered = value["tools"]
        .as_array()
        .unwrap_or_else(|| panic!("case {case}: tools should be an array"))
        .iter()
        .filter_map(|tool| {
            tool.get("function")
                .and_then(|function| function.get("name"))
                .and_then(Value::as_str)
        })
        .collect::<Vec<_>>();
    for call in tool_calls_for_sequence(sequence) {
        assert!(
            offered.contains(&call.name),
            "case {case}: request did not offer {} in {offered:?}",
            call.name
        );
    }
}

fn tool_messages_from_followup(body: &str, case: &str) -> Vec<Value> {
    let value: Value = serde_json::from_str(body).unwrap();
    value["messages"]
        .as_array()
        .unwrap_or_else(|| panic!("case {case}: messages should be an array"))
        .iter()
        .filter(|message| message["role"].as_str() == Some("tool"))
        .cloned()
        .collect()
}

fn assert_fragments_in_tool_messages(
    messages: &[Value],
    fragments: &str,
    expected_is_error: Option<bool>,
    case: &str,
) {
    for fragment in fragments.split('|').filter(|fragment| !fragment.is_empty()) {
        let found = messages.iter().any(|message| {
            message["content"]
                .as_str()
                .is_some_and(|content| content.contains(fragment))
                && expected_is_error
                    .is_none_or(|expected| message["is_error"].as_bool() == Some(expected))
        });
        assert!(
            found,
            "case {case}: missing fragment {fragment:?} with is_error={expected_is_error:?} in {messages:?}"
        );
    }
}

fn assert_turn_bracket(events: &[SessionEvent], case: &str) {
    assert_eq!(
        events.first(),
        Some(&SessionEvent::TurnStarted { turn: TurnId(1) }),
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

fn parse_usize(row: &BTreeMap<String, String>, field_name: &str) -> usize {
    field(row, field_name).parse::<usize>().unwrap()
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .trim()
}

fn builtin_replies() -> MutexGuard<'static, VecDeque<String>> {
    BUILTIN_REPLIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn builtin_request_bodies() -> MutexGuard<'static, Vec<String>> {
    BUILTIN_REQUEST_BODIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
