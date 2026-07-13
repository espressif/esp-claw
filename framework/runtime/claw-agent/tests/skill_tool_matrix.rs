#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::collections::{BTreeMap, VecDeque};
use std::sync::{Mutex, MutexGuard};

use claw_agent::{AgentPersistenceConfig, AgentSystem, IterationId, Message, SessionEvent, TurnId};
use claw_interface::{
    Cancel, ClawFs, ClawHttp, DiskFs, HttpJsonRequest, HttpResponse, HttpResponseFuture,
    HttpStatusCode, ImmediateTimer, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{assistant_text, csv_dicts, drain_until_turn_ended, llm_config};
use tempdir::TempDir;

type SkillToolSystem = AgentSystem<DiskFs, Sse<SkillToolHttp>, ImmediateTimer>;

static SKILL_TOOL_LOCK: Mutex<()> = Mutex::new(());
static SKILL_TOOL_STATE: Mutex<Option<SkillToolCaseState>> = Mutex::new(None);

#[test]
fn skill_tools_csv_matrix_scans_roots_reloads_and_activates_documents() {
    let _lock = SKILL_TOOL_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());

    for row in csv_dicts(include_str!("fixtures/skill_tool_cases.csv")) {
        let fixture = Fixture::from_row(&row);
        let temp = TempDir::new("claw-agent-skill-tool").unwrap();
        let base = temp.path().to_string_lossy();
        let persistence_root = format!("{base}/persist");
        let data_root = format!("{base}/skill-data");
        let system_root = format!("{base}/skill-system");
        let runtime_root = format!("{base}/skill-runtime");
        install_initial_skills(&data_root, &system_root);
        materialize_root(&runtime_root);

        install_case(fixture.clone());
        let system = SkillToolSystem::new::<StdThread, TokioExecutor>(AgentPersistenceConfig {
            persistence_root,
            skill_roots: vec![data_root.clone(), system_root.clone(), runtime_root.clone()],
        })
        .unwrap();
        system
            .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
            .unwrap();
        install_runtime_skill(&runtime_root);

        let session = system.new_session(claw_agent::SessionPersistence::Persistent);
        let (control, mut events) = system.open_session(session).unwrap();
        block_on(control.submit(Message::text(format!("run skill matrix {}", fixture.case))))
            .unwrap();
        let events = drain_until_turn_ended(&mut events);

        assert_turn_bracket(&events, &fixture.case);
        assert_eq!(
            iteration_ids(&events),
            vec![IterationId(0), IterationId(1)],
            "case {}",
            fixture.case
        );
        assert_eq!(
            tools_events(&events),
            vec![
                "skill_list".to_string(),
                "skill_activate".to_string(),
                "skill_activate".to_string(),
                "skill_reload".to_string(),
                "skill_list".to_string(),
                "skill_activate".to_string(),
            ],
            "case {}",
            fixture.case
        );
        assert_eq!(
            output_fragments(&events),
            vec![fixture.final_output.clone()],
            "case {}",
            fixture.case
        );
        assert!(
            error_messages(&events).is_empty(),
            "case {}: {events:?}",
            fixture.case
        );

        assert_request_history(&fixture);
    }
}

#[derive(Default)]
struct SkillToolHttp;

impl ClawHttp for SkillToolHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        Box::pin(async move {
            let response = if is_agent_iteration_request(&body) {
                let index = {
                    let mut state = state();
                    let state = state.as_mut().expect("skill tool test case installed");
                    state.request_bodies.push_back(body.clone());
                    let index = state.root_requests;
                    state.root_requests = state.root_requests.saturating_add(1);
                    index
                };
                match index {
                    0 => assistant_tool_calls(vec![
                        call("call_list_before_reload", "skill_list", json!({})),
                        call(
                            "call_activate_alpha",
                            "skill_activate",
                            json!({ "skill_id": "alpha" }),
                        ),
                        call(
                            "call_activate_missing",
                            "skill_activate",
                            json!({ "skill_id": "ghost" }),
                        ),
                        call("call_reload", "skill_reload", json!({})),
                        call("call_list_after_reload", "skill_list", json!({})),
                        call(
                            "call_activate_gamma",
                            "skill_activate",
                            json!({ "skill_id": "gamma" }),
                        ),
                    ]),
                    1 => {
                        let final_output = current_fixture()
                            .map(|fixture| fixture.final_output)
                            .expect("skill tool fixture is installed");
                        assistant_text(&final_output)
                    }
                    other => panic!("unexpected skill tool root request index {other}: {body}"),
                }
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
    final_output: String,
    initial_context_has: String,
    initial_context_lacks: String,
    tool_ok_fragments: String,
    tool_error_fragments: String,
}

impl Fixture {
    fn from_row(row: &BTreeMap<String, String>) -> Self {
        Self {
            case: field(row, "case").to_string(),
            final_output: field(row, "final_output").to_string(),
            initial_context_has: field(row, "initial_context_has").to_string(),
            initial_context_lacks: field(row, "initial_context_lacks").to_string(),
            tool_ok_fragments: field(row, "tool_ok_fragments").to_string(),
            tool_error_fragments: field(row, "tool_error_fragments").to_string(),
        }
    }
}

struct SkillToolCaseState {
    fixture: Fixture,
    root_requests: usize,
    request_bodies: VecDeque<String>,
}

fn install_case(fixture: Fixture) {
    *state() = Some(SkillToolCaseState {
        fixture,
        root_requests: 0,
        request_bodies: VecDeque::new(),
    });
}

fn install_initial_skills(data_root: &str, system_root: &str) {
    write_skill(
        system_root,
        "alpha",
        "system alpha should be shadowed",
        "readonly",
        "Alpha system body should not appear",
    );
    write_skill(
        data_root,
        "alpha",
        "data alpha override",
        "runtime",
        "Alpha data body from {CUR_SKILL_DIR}/asset.txt",
    );
    write_skill(
        system_root,
        "beta",
        "system beta skill",
        "readonly",
        "Beta system body",
    );
}

fn install_runtime_skill(runtime_root: &str) {
    write_skill(
        runtime_root,
        "gamma",
        "gamma runtime skill",
        "runtime",
        "Gamma body from {CUR_SKILL_DIR}/notes.md",
    );
}

fn write_skill(root: &str, id: &str, description: &str, manage_mode: &str, body: &str) {
    materialize_root(root);
    let document = format!(
        "---\n{}\n---\n{}\n",
        json!({
            "name": id,
            "description": description,
            "author": "test-suite",
            "metadata": {
                "cap_groups": ["test-cap"],
                "manage_mode": manage_mode,
                "category": ["test"],
                "peripherals": [],
                "tags": ["matrix"],
            },
        }),
        body
    );
    DiskFs::write_atomic(&format!("{root}/{id}/SKILL.md"), document.as_bytes()).unwrap();
}

fn materialize_root(root: &str) {
    DiskFs::create_dir_all(root).unwrap();
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

fn call(id: &str, name: &str, args: Value) -> Value {
    json!({
        "id": id,
        "type": "function",
        "function": {
            "name": name,
            "arguments": args.to_string(),
        },
    })
}

fn assert_request_history(fixture: &Fixture) {
    let bodies = recorded_request_bodies();
    assert_eq!(
        bodies.len(),
        2,
        "case {}: expected initial and follow-up requests: {bodies:?}",
        fixture.case
    );
    assert_fragments(&bodies[0], &fixture.initial_context_has, &fixture.case);
    assert_absent_fragments(&bodies[0], &fixture.initial_context_lacks, &fixture.case);

    let tool_messages = tool_messages_by_id(&bodies[1]);
    let list_before = tool_content(&tool_messages, "call_list_before_reload", &fixture.case);
    assert_fragments(
        list_before,
        "\"id\":\"alpha\"|\"id\":\"beta\"",
        &fixture.case,
    );
    assert_absent_fragments(list_before, "\"id\":\"gamma\"", &fixture.case);

    let alpha = tool_content(&tool_messages, "call_activate_alpha", &fixture.case);
    assert_fragments(
        alpha,
        "Alpha data body|/skill-data/alpha|<skill_content name=\"alpha\">",
        &fixture.case,
    );
    assert_absent_fragments(alpha, "Alpha system body should not appear", &fixture.case);

    let missing = tool_content(&tool_messages, "call_activate_missing", &fixture.case);
    assert_fragments(missing, &fixture.tool_error_fragments, &fixture.case);

    let reload = tool_content(&tool_messages, "call_reload", &fixture.case);
    assert_fragments(reload, "Skills refreshed", &fixture.case);

    let list_after = tool_content(&tool_messages, "call_list_after_reload", &fixture.case);
    assert_fragments(
        list_after,
        "\"id\":\"gamma\"|gamma runtime skill",
        &fixture.case,
    );

    let gamma = tool_content(&tool_messages, "call_activate_gamma", &fixture.case);
    assert_fragments(gamma, &fixture.tool_ok_fragments, &fixture.case);
}

fn tool_messages_by_id(body: &str) -> BTreeMap<String, Value> {
    let value: Value = serde_json::from_str(body).unwrap();
    value["messages"]
        .as_array()
        .into_iter()
        .flatten()
        .filter(|message| message["role"].as_str() == Some("tool"))
        .filter_map(|message| {
            Some((
                message["tool_call_id"].as_str()?.to_string(),
                message.clone(),
            ))
        })
        .collect()
}

fn tool_content<'a>(messages: &'a BTreeMap<String, Value>, id: &str, case: &str) -> &'a str {
    messages
        .get(id)
        .and_then(|message| message["content"].as_str())
        .unwrap_or_else(|| panic!("case {case}: missing tool message {id} in {messages:?}"))
}

fn assert_fragments(text: &str, fragments: &str, case: &str) {
    for fragment in fragments.split('|').filter(|fragment| !fragment.is_empty()) {
        assert!(
            text.contains(fragment),
            "case {case}: missing fragment {fragment:?} in {text:?}"
        );
    }
}

fn assert_absent_fragments(text: &str, fragments: &str, case: &str) {
    for fragment in fragments.split('|').filter(|fragment| !fragment.is_empty()) {
        assert!(
            !text.contains(fragment),
            "case {case}: unexpected fragment {fragment:?} in {text:?}"
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

fn recorded_request_bodies() -> Vec<String> {
    state()
        .as_ref()
        .expect("skill tool test case installed")
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

fn state() -> MutexGuard<'static, Option<SkillToolCaseState>> {
    SKILL_TOOL_STATE
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}
