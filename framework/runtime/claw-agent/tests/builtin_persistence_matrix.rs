#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::collections::{BTreeMap, VecDeque};
use std::sync::{Mutex, MutexGuard};

use claw_agent::{AgentSystem, IterationId, SessionEvent, TurnCause, TurnId};
use claw_interface::{
    Cancel, ClawFs, ClawHttp, DiskFs, HttpJsonRequest, HttpResponse, HttpResponseFuture,
    HttpStatusCode, ImmediateTimer, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{assistant_text, csv_dicts, drain_until_turn_ended, llm_config, persistence};
use tempdir::TempDir;

type BuiltinPersistenceSystem = AgentSystem<DiskFs, Sse<BuiltinPersistenceHttp>, ImmediateTimer>;

static BUILTIN_PERSISTENCE_LOCK: Mutex<()> = Mutex::new(());
static BUILTIN_PERSISTENCE_REPLIES: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static BUILTIN_PERSISTENCE_REQUESTS: Mutex<Vec<String>> = Mutex::new(Vec::new());

#[test]
fn builtin_profile_and_memory_csv_matrix_survives_disk_rebuild() {
    let _lock = BUILTIN_PERSISTENCE_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/builtin_persistence_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        let temp = TempDir::new("claw-agent-builtin-persistence").unwrap();
        let root = temp.path().to_string_lossy().into_owned();

        let setup = run_phase(
            &root,
            format!("setup durable builtin state {}", fixture.case),
            vec![
                call(
                    "call_profile_replace",
                    "profile_replace",
                    json!({
                        "document": "user_profile",
                        "content": "Persistent user profile",
                    }),
                ),
                call(
                    "call_memory_global_store",
                    "memory_store",
                    json!({
                        "content": "Persistent global fact",
                        "tags": ["fact"],
                        "keywords": ["persistent", "global"],
                    }),
                ),
                call(
                    "call_memory_agent_store",
                    "memory_store",
                    json!({
                        "content": "Persistent task note",
                        "tags": ["task"],
                        "keywords": ["persistent", "task"],
                    }),
                ),
            ],
            &fixture.setup_final,
        );
        assert_phase(&setup, &fixture.setup_final, &fixture.case);
        assert_followup_tool_fragments(&setup.requests, &fixture.setup_fragments, &fixture.case);

        assert_disk_file_contains(&root, "profile/user.md", "Persistent user profile");
        assert_disk_file_contains(
            &root,
            "long_term/global/memory_records.jsonl",
            "Persistent global fact",
        );
        assert_disk_file_contains(
            &root,
            "long_term/agents/conversation/memory_records.jsonl",
            "Persistent task note",
        );

        let verify = run_phase(
            &root,
            format!("verify durable builtin state {}", fixture.case),
            vec![
                call(
                    "call_profile_read",
                    "profile_read",
                    json!({ "document": "user_profile" }),
                ),
                call(
                    "call_memory_global_recall",
                    "memory_recall",
                    json!({ "labels": ["fact"], "query": "global", "limit": 5 }),
                ),
                call(
                    "call_memory_agent_recall",
                    "memory_recall",
                    json!({ "labels": ["task"], "query": "task", "limit": 5 }),
                ),
                call("call_memory_list", "memory_list", json!({ "limit": 5 })),
            ],
            &fixture.verify_final,
        );
        assert_phase(&verify, &fixture.verify_final, &fixture.case);
        assert_fragments(
            &verify.requests[0],
            &fixture.verify_context_fragments,
            &fixture.case,
        );
        assert_followup_tool_fragments(&verify.requests, &fixture.verify_fragments, &fixture.case);
    }
}

#[test]
fn builtin_memory_journal_torn_tail_keeps_committed_records_after_rebuild() {
    let _lock = BUILTIN_PERSISTENCE_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let temp = TempDir::new("claw-agent-memory-torn-tail").unwrap();
    let root = temp.path().to_string_lossy().into_owned();
    let case = "memory-journal-torn-tail";

    let setup = run_phase(
        &root,
        "setup memory before torn tail".to_string(),
        vec![call(
            "call_memory_store",
            "memory_store",
            json!({
                "content": "Torn-tail durable fact",
                "tags": ["fact"],
                "keywords": ["torn", "durable"],
            }),
        )],
        "setup torn-tail",
    );
    assert_phase(&setup, "setup torn-tail", case);
    assert_followup_tool_fragments(&setup.requests, "Stored memory g-0.", case);
    assert_disk_file_contains(
        &root,
        "long_term/global/memory_records.jsonl",
        "Torn-tail durable fact",
    );

    DiskFs::append(
        &format!("{root}/long_term/global/memory_records.jsonl"),
        br#"{"torn":"record""#,
    )
    .unwrap();

    let verify = run_phase(
        &root,
        "verify memory after torn tail".to_string(),
        vec![call(
            "call_memory_recall",
            "memory_recall",
            json!({ "labels": ["fact"], "query": "durable", "limit": 5 }),
        )],
        "verify torn-tail",
    );
    assert_phase(&verify, "verify torn-tail", case);
    assert_fragments(
        &verify.requests[0],
        "Shared long-term memory topics: fact",
        case,
    );
    assert_followup_tool_fragments(
        &verify.requests,
        "Recalled memories|g-0|Torn-tail durable fact",
        case,
    );
}

#[test]
fn builtin_profile_clear_and_memory_update_forget_survive_disk_rebuilds() {
    let _lock = BUILTIN_PERSISTENCE_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    let temp = TempDir::new("claw-agent-builtin-mutation-persistence").unwrap();
    let root = temp.path().to_string_lossy().into_owned();
    let case = "profile-clear-memory-update-forget";

    let seed = run_phase(
        &root,
        "seed mutable builtin state".to_string(),
        vec![
            call(
                "call_profile_replace",
                "profile_replace",
                json!({
                    "document": "user_profile",
                    "content": "Mutable profile content",
                }),
            ),
            call(
                "call_memory_store",
                "memory_store",
                json!({
                    "content": "Mutable global fact",
                    "tags": ["fact"],
                    "keywords": ["mutable"],
                }),
            ),
        ],
        "seeded mutable state",
    );
    assert_phase(&seed, "seeded mutable state", case);
    assert_followup_tool_fragments(
        &seed.requests,
        "Replaced profile document user_profile.|Stored memory g-0.",
        case,
    );
    assert_disk_file_contains(&root, "profile/user.md", "Mutable profile content");
    assert_disk_file_contains(
        &root,
        "long_term/global/memory_records.jsonl",
        "Mutable global fact",
    );

    let mutate = run_phase(
        &root,
        "mutate rebuilt builtin state".to_string(),
        vec![
            call(
                "call_profile_clear",
                "profile_clear",
                json!({ "document": "user_profile" }),
            ),
            call(
                "call_profile_read",
                "profile_read",
                json!({ "document": "user_profile" }),
            ),
            call(
                "call_memory_update",
                "memory_update",
                json!({
                    "id": "g-0",
                    "content": "Updated durable global fact",
                    "tags": ["fact"],
                    "keywords": ["updated", "durable"],
                }),
            ),
            call(
                "call_memory_recall_updated",
                "memory_recall",
                json!({ "labels": ["fact"], "query": "updated", "limit": 5 }),
            ),
        ],
        "mutated rebuilt state",
    );
    assert_phase(&mutate, "mutated rebuilt state", case);
    assert_followup_tool_fragments(
        &mutate.requests,
        "Cleared profile document user_profile.|Profile document user_profile is empty.|Updated memory g-0.|Updated durable global fact",
        case,
    );
    assert_disk_file_equals(&root, "profile/user.md", "");
    assert_disk_file_contains(
        &root,
        "long_term/global/memory_records.jsonl",
        "Updated durable global fact",
    );

    let verify_and_forget = run_phase(
        &root,
        "verify and forget rebuilt mutation state".to_string(),
        vec![
            call(
                "call_profile_read_after_rebuild",
                "profile_read",
                json!({ "document": "user_profile" }),
            ),
            call(
                "call_memory_recall_old",
                "memory_recall",
                json!({ "labels": ["fact"], "query": "mutable", "limit": 5 }),
            ),
            call(
                "call_memory_list_updated",
                "memory_list",
                json!({ "limit": 5 }),
            ),
            call(
                "call_memory_forget",
                "memory_forget",
                json!({ "id": "g-0" }),
            ),
            call(
                "call_memory_list_after_forget",
                "memory_list",
                json!({ "limit": 5 }),
            ),
        ],
        "forgot rebuilt memory",
    );
    assert_phase(&verify_and_forget, "forgot rebuilt memory", case);
    assert_followup_tool_fragments(
        &verify_and_forget.requests,
        "Profile document user_profile is empty.|No matching memories.|Updated durable global fact|Forgot memory g-0.",
        case,
    );

    let verify_empty = run_phase(
        &root,
        "verify empty mutation state after rebuild".to_string(),
        vec![
            call(
                "call_profile_read_empty",
                "profile_read",
                json!({ "document": "user_profile" }),
            ),
            call(
                "call_memory_list_empty",
                "memory_list",
                json!({ "limit": 5 }),
            ),
        ],
        "verified empty mutation state",
    );
    assert_phase(&verify_empty, "verified empty mutation state", case);
    assert_followup_tool_fragments(
        &verify_empty.requests,
        "Profile document user_profile is empty.|No matching memories.",
        case,
    );
    assert_not_contains(&verify_empty.requests[0], "Mutable profile content", case);
    assert_not_contains(
        &verify_empty.requests[0],
        "Updated durable global fact",
        case,
    );
}

#[derive(Default)]
struct BuiltinPersistenceHttp;

impl ClawHttp for BuiltinPersistenceHttp {
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
                    .expect("builtin persistence script exhausted")
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
    setup_final: String,
    verify_final: String,
    setup_fragments: String,
    verify_fragments: String,
    verify_context_fragments: String,
}

impl Fixture {
    fn from_row(row: &BTreeMap<String, String>) -> Self {
        Self {
            case: field(row, "case").to_string(),
            setup_final: field(row, "setup_final").to_string(),
            verify_final: field(row, "verify_final").to_string(),
            setup_fragments: field(row, "setup_fragments").to_string(),
            verify_fragments: field(row, "verify_fragments").to_string(),
            verify_context_fragments: field(row, "verify_context_fragments").to_string(),
        }
    }
}

#[derive(Clone)]
struct ToolCallSpec {
    id: &'static str,
    name: &'static str,
    args: Value,
}

struct PhaseResult {
    events: Vec<SessionEvent>,
    requests: Vec<String>,
}

fn run_phase(
    root: &str,
    input: String,
    calls: Vec<ToolCallSpec>,
    final_output: &str,
) -> PhaseResult {
    install_replies(vec![
        assistant_tool_calls(calls),
        assistant_text(final_output),
    ]);

    let system =
        BuiltinPersistenceSystem::new::<StdThread, TokioExecutor>(persistence(root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    let session = system.new_session();
    let (control, mut events) = system.open_session(session).unwrap();
    block_on(control.submit(input)).unwrap();
    let events = drain_until_turn_ended(&mut events);
    drop(system);

    PhaseResult {
        events,
        requests: requests().clone(),
    }
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

fn assert_phase(result: &PhaseResult, expected_output: &str, case: &str) {
    assert_eq!(
        result.events.first(),
        Some(&SessionEvent::TurnStarted {
            turn: TurnId(1),
            cause: TurnCause::UserSubmit,
        }),
        "case {case}"
    );
    assert_eq!(
        result.events.last(),
        Some(&SessionEvent::TurnEnded { turn: TurnId(1) }),
        "case {case}"
    );
    assert_eq!(
        iteration_ids(&result.events),
        vec![IterationId(0), IterationId(1)],
        "case {case}"
    );
    assert_eq!(
        output_fragments(&result.events),
        vec![expected_output.to_string()],
        "case {case}"
    );
    assert!(
        error_messages(&result.events).is_empty(),
        "case {case}: {:?}",
        result.events
    );
    assert_eq!(
        result.requests.len(),
        2,
        "case {case}: expected initial and follow-up LLM requests"
    );
}

fn assert_followup_tool_fragments(requests: &[String], fragments: &str, case: &str) {
    let followup = requests
        .get(1)
        .unwrap_or_else(|| panic!("case {case}: missing follow-up request"));
    let content = tool_message_content(followup, case).join("\n");
    assert_fragments(&content, fragments, case);
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

fn assert_fragments(text: &str, fragments: &str, case: &str) {
    for fragment in fragments.split('|').filter(|fragment| !fragment.is_empty()) {
        assert!(
            text.contains(fragment),
            "case {case}: missing fragment {fragment:?} in {text:?}"
        );
    }
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

fn assert_disk_file_equals(root: &str, relative: &str, expected: &str) {
    let path = format!("{root}/{relative}");
    let bytes = DiskFs::read(&path).unwrap_or_else(|error| panic!("{path}: {error}"));
    let text = String::from_utf8(bytes).unwrap_or_else(|error| panic!("{path}: {error}"));
    assert_eq!(text, expected, "expected {path} to equal {expected:?}");
}

fn assert_not_contains(text: &str, fragment: &str, case: &str) {
    assert!(
        !text.contains(fragment),
        "case {case}: unexpected fragment {fragment:?} in {text:?}"
    );
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
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

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .trim()
}

fn replies() -> MutexGuard<'static, VecDeque<String>> {
    BUILTIN_PERSISTENCE_REPLIES
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn requests() -> MutexGuard<'static, Vec<String>> {
    BUILTIN_PERSISTENCE_REQUESTS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
