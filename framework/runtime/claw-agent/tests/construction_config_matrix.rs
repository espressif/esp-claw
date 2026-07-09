mod support;

use std::collections::BTreeMap;

use claw_agent::{AgentPersistenceConfig, AgentSystem, SessionEvent};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::{
    Cancel, ClawHttp, DiskFs, HttpJsonRequest, HttpResponse, HttpResponseFuture, HttpStatusCode,
    ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use serde_json::Value;
use support::{assistant_text, csv_dicts, drain_until_turn_ended, mem_root};
use tempdir::TempDir;

type MemConstructionSystem = AgentSystem<MemFs, ConstructionHttp, ImmediateTimer>;
type DiskConstructionSystem = AgentSystem<DiskFs, ConstructionHttp, ImmediateTimer>;

#[test]
fn construction_csv_config_matrix_validates_llm_config_and_skill_roots() {
    for row in csv_dicts(include_str!("fixtures/construction_config_cases.csv")) {
        let case = field(&row, "case");
        let config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            field(&row, "api_key"),
            field(&row, "model"),
            field(&row, "base_url"),
        );
        let expect_ok = parse_bool(field(&row, "expect_ok"));

        match field(&row, "fs") {
            "mem" => {
                MemFs::default();
                let root = mem_root("construction-config");
                let persistence = mem_persistence(&root, field(&row, "skill_roots_mode"));
                assert_construction_case::<MemConstructionSystem>(
                    case,
                    config,
                    persistence,
                    expect_ok,
                    field(&row, "error_contains"),
                );
            }
            "disk" => {
                let temp = TempDir::new("claw-agent-construction-config").unwrap();
                let root = temp.path().join("agent").to_string_lossy().into_owned();
                let persistence = disk_persistence(&root, &temp, field(&row, "skill_roots_mode"));
                assert_construction_case::<DiskConstructionSystem>(
                    case,
                    config,
                    persistence,
                    expect_ok,
                    field(&row, "error_contains"),
                );
            }
            other => panic!("unsupported fs in fixture: {other}"),
        }
    }
}

#[derive(Default)]
struct ConstructionHttp;

impl ClawHttp for ConstructionHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            let body = if is_agent_iteration_request(request.body) {
                assistant_text("construction-ok")
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

trait ConstructionSystem: Sized {
    fn build(
        config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> Result<Self, claw_agent::AgentError>;
    fn new_session(&self) -> claw_agent::SessionId;
    fn open_session(
        &self,
        session: claw_agent::SessionId,
    ) -> claw_agent::AgentResult<(claw_agent::SessionControl, claw_agent::SessionEventStream)>;
}

impl ConstructionSystem for MemConstructionSystem {
    fn build(
        config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> Result<Self, claw_agent::AgentError> {
        Self::new::<StdThread, TokioExecutor>(config, persistence)
    }

    fn new_session(&self) -> claw_agent::SessionId {
        self.new_session()
    }

    fn open_session(
        &self,
        session: claw_agent::SessionId,
    ) -> claw_agent::AgentResult<(claw_agent::SessionControl, claw_agent::SessionEventStream)> {
        self.open_session(session)
    }
}

impl ConstructionSystem for DiskConstructionSystem {
    fn build(
        config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> Result<Self, claw_agent::AgentError> {
        Self::new::<StdThread, TokioExecutor>(config, persistence)
    }

    fn new_session(&self) -> claw_agent::SessionId {
        self.new_session()
    }

    fn open_session(
        &self,
        session: claw_agent::SessionId,
    ) -> claw_agent::AgentResult<(claw_agent::SessionControl, claw_agent::SessionEventStream)> {
        self.open_session(session)
    }
}

fn assert_construction_case<System: ConstructionSystem>(
    case: &str,
    config: ClawApiConfig,
    persistence: AgentPersistenceConfig,
    expect_ok: bool,
    error_contains: &str,
) {
    match System::build(config, persistence) {
        Ok(system) if expect_ok => {
            let session = system.new_session();
            let (control, mut events) = system.open_session(session).unwrap();
            block_on(control.submit(format!("construction config {case}"))).unwrap();
            let events = drain_until_turn_ended(&mut events);
            assert!(
                events.iter().any(
                    |event| matches!(event, SessionEvent::Output { text } if text == "construction-ok")
                ),
                "case {case}: {events:?}"
            );
        }
        Ok(_) => panic!("case {case}: construction should have failed"),
        Err(error) if expect_ok => panic!("case {case}: construction failed: {error}"),
        Err(error) => assert!(
            error.to_string().contains(error_contains),
            "case {case}: {error} should contain {error_contains:?}"
        ),
    }
}

fn mem_persistence(root: &str, mode: &str) -> AgentPersistenceConfig {
    AgentPersistenceConfig {
        persistence_root: root.to_string(),
        skill_roots: skill_roots_for_mode(root, mode),
    }
}

fn disk_persistence(root: &str, temp: &TempDir, mode: &str) -> AgentPersistenceConfig {
    let skill_roots = match mode {
        "existing_empty" => {
            let skill_root = temp.path().join("skills");
            std::fs::create_dir_all(&skill_root).unwrap();
            vec![skill_root.to_string_lossy().into_owned()]
        }
        _ => skill_roots_for_mode(root, mode),
    };
    AgentPersistenceConfig {
        persistence_root: root.to_string(),
        skill_roots,
    }
}

fn skill_roots_for_mode(root: &str, mode: &str) -> Vec<String> {
    match mode {
        "none" => Vec::new(),
        "missing" => vec![format!("{root}/missing-skills")],
        "multiple_missing" => vec![
            format!("{root}/missing-skills-a"),
            format!("{root}/missing-skills-b"),
        ],
        other => panic!("unsupported skill root mode: {other}"),
    }
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

fn parse_bool(value: &str) -> bool {
    match value {
        "true" => true,
        "false" => false,
        other => panic!("invalid bool in fixture: {other}"),
    }
}

fn field<'a>(row: &'a BTreeMap<String, String>, name: &str) -> &'a str {
    row.get(name)
        .unwrap_or_else(|| panic!("missing csv column {name}"))
        .as_str()
}
