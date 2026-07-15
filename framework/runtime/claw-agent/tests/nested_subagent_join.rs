#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::sync::Mutex;

use claw_agent::{AgentSystem, Message, SessionEvent, StreamPart, TurnOrigin};
use claw_interface::{
    Cancel, ClawHttp, ClawTimer, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    MemFs, StdThread, TimerFuture, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::{json, Value};
use support::{drain_until_turn_ended, llm_config, mem_root, persistence};

type NestedSystem = AgentSystem<MemFs, Sse<NestedHttp>, PendingTimer>;

static TEST_LOCK: Mutex<()> = Mutex::new(());
static REQUESTS: Mutex<Vec<String>> = Mutex::new(Vec::new());

const EPSILON_GOAL: &str = "epsilon parent goal";
const LEAF_ONE_GOAL: &str = "leaf one goal";
const LEAF_TWO_GOAL: &str = "leaf two goal";
const LEAF_ONE_RESULT: &str = "leaf-one-done";
const LEAF_TWO_RESULT: &str = "leaf-two-done";
const EPSILON_RESULT: &str = "epsilon-aggregated-both-results";

#[test]
fn nested_background_children_join_before_their_parent_reports_upward() {
    let _lock = TEST_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    REQUESTS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .clear();

    let root = mem_root("nested-subagent-join");
    let system = NestedSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    let session = system.new_session(claw_agent::SessionPersistence::Persistent);
    let (control, mut events) = system.open_session(session).unwrap();

    block_on(control.submit(Message::text("delegate nested work"))).unwrap();
    let delegated = drain_until_turn_ended(&mut events);
    assert_eq!(output_fragments(&delegated), vec!["epsilon requested"]);

    let completed = drain_until_turn_ended(&mut events);
    assert!(matches!(
        completed.first(),
        Some(SessionEvent::TurnStarted {
            origin: TurnOrigin::Subagent { .. },
            ..
        })
    ));
    assert_eq!(
        output_fragments(&completed),
        vec!["root received epsilon aggregate"]
    );

    let requests = REQUESTS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .clone();
    assert!(requests.iter().any(|body| {
        body.contains(EPSILON_GOAL)
            && body.contains(LEAF_ONE_RESULT)
            && body.contains(LEAF_TWO_RESULT)
    }));
    assert!(requests
        .iter()
        .any(|body| body.contains(EPSILON_RESULT) && body.contains("user-facing assistant")));
}

#[derive(Default)]
struct PendingTimer;

impl ClawTimer for PendingTimer {
    fn sleep<'a>(
        &'a mut self,
        _duration: core::time::Duration,
        _cancel: Cancel<'a>,
    ) -> TimerFuture<'a> {
        Box::pin(core::future::pending())
    }
}

#[derive(Default)]
struct NestedHttp;

impl ClawHttp for NestedHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        let body = request.body.to_owned();
        Box::pin(async move {
            REQUESTS
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .push(body.clone());
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body: response_for_request(&body),
            })
        })
    }
}

fn response_for_request(body: &str) -> String {
    let value: Value = serde_json::from_str(body).expect("valid request body");
    if value.get("response_format").is_some() || value.get("tools").is_none() {
        return assistant_text("[]");
    }
    let system = value["messages"]
        .as_array()
        .and_then(|messages| messages.first())
        .and_then(|message| message["content"].as_str())
        .unwrap_or_default();

    if system.contains("user-facing assistant") {
        return root_response(body);
    }
    if system.contains("subagent spawned by the root agent") {
        return worker_response(body);
    }
    panic!("unexpected request: {body}");
}

fn root_response(body: &str) -> String {
    if body.contains(EPSILON_RESULT) {
        return assistant_text("root received epsilon aggregate");
    }
    if has_tool_message(body) {
        return assistant_text("epsilon requested");
    }
    assistant_tool_calls(vec![tool_call(
        "spawn_epsilon",
        "subagent_spawn",
        json!({
            "kind": "worker",
            "name": "epsilon",
            "goal": EPSILON_GOAL,
            "foreground": false,
            "timeout_ms": 60_000,
        }),
    )])
}

fn worker_response(body: &str) -> String {
    if body.contains(EPSILON_GOAL) {
        if body.contains(LEAF_ONE_RESULT) && body.contains(LEAF_TWO_RESULT) {
            return assistant_text(EPSILON_RESULT);
        }
        if body.contains(LEAF_ONE_RESULT)
            || body.contains(LEAF_TWO_RESULT)
            || has_tool_message(body)
        {
            return assistant_text("children spawned; waiting for their results");
        }
        return assistant_tool_calls(vec![
            tool_call(
                "spawn_leaf_one",
                "subagent_spawn",
                json!({
                    "kind": "worker",
                    "name": "leaf-one",
                    "goal": LEAF_ONE_GOAL,
                    "foreground": false,
                    "timeout_ms": 60_000,
                }),
            ),
            tool_call(
                "spawn_leaf_two",
                "subagent_spawn",
                json!({
                    "kind": "worker",
                    "name": "leaf-two",
                    "goal": LEAF_TWO_GOAL,
                    "foreground": false,
                    "timeout_ms": 60_000,
                }),
            ),
        ]);
    }
    if body.contains(LEAF_ONE_GOAL) {
        return assistant_text(LEAF_ONE_RESULT);
    }
    if body.contains(LEAF_TWO_GOAL) {
        return assistant_text(LEAF_TWO_RESULT);
    }
    panic!("unknown worker goal: {body}");
}

fn has_tool_message(body: &str) -> bool {
    let value: Value = serde_json::from_str(body).expect("valid request body");
    value["messages"]
        .as_array()
        .into_iter()
        .flatten()
        .any(|message| message["role"].as_str() == Some("tool"))
}

fn assistant_text(text: &str) -> String {
    json!({
        "choices": [{
            "message": { "role": "assistant", "content": text }
        }]
    })
    .to_string()
}

fn assistant_tool_calls(calls: Vec<Value>) -> String {
    json!({
        "choices": [{
            "message": { "role": "assistant", "tool_calls": calls }
        }]
    })
    .to_string()
}

fn tool_call(id: &str, name: &str, arguments: Value) -> Value {
    json!({
        "id": id,
        "type": "function",
        "function": {
            "name": name,
            "arguments": arguments.to_string(),
        }
    })
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
