#![allow(clippy::unwrap_used)]

mod support;
use support::Sse;

use std::collections::{BTreeMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::thread;

use claw_agent::{
    AgentSystem, Message, SessionControl, SessionControlError, SessionEvent, SessionEventStream,
    SessionId,
};
use claw_interface::{
    Cancel, ClawFs, ClawHttp, DiskFs, HttpError, HttpJsonRequest, HttpResponse, HttpResponseFuture,
    HttpStatusCode, ImmediateTimer, MemFs, StdThread, TokioExecutor,
};
use futures_lite::future::block_on;
use futures_lite::StreamExt;
use support::{
    assistant_text, build_mem_system, csv_dicts, drain_until_turn_ended, llm_config, mem_root,
    persistence,
};
use tempdir::TempDir;

type MemStressSystem = AgentSystem<MemFs, Sse<StressScriptHttp>, ImmediateTimer>;
type DiskStressSystem = AgentSystem<DiskFs, Sse<StressScriptHttp>, ImmediateTimer>;
type YieldingAgentSystem = AgentSystem<MemFs, Sse<YieldingCountingHttp>, ImmediateTimer>;

static STRESS_OUTPUTS: Mutex<VecDeque<String>> = Mutex::new(VecDeque::new());
static YIELDING_HTTP_CALLS: AtomicUsize = AtomicUsize::new(0);
static YIELDING_HTTP_POLLS: AtomicUsize = AtomicUsize::new(0);

#[test]
fn stress_csv_session_turn_matrix_preserves_outputs_and_rebuilds() {
    for row in csv_dicts(include_str!("fixtures/stress_session_matrix.csv")) {
        let case = field(&row, "case");
        let sessions = parse_usize(&row, "sessions");
        let turns_per_session = parse_usize(&row, "turns_per_session");
        let output_bytes = parse_usize(&row, "output_bytes");
        let reopen_each_turn = parse_bool(field(&row, "reopen_each_turn"));
        let rebuild_after = parse_bool(field(&row, "rebuild_after"));
        let expected_outputs = expected_outputs(case, sessions, turns_per_session, output_bytes);

        match field(&row, "fs") {
            "mem" => run_mem_stress_case(
                case,
                sessions,
                turns_per_session,
                reopen_each_turn,
                &expected_outputs,
            ),
            "disk" => run_disk_stress_case(
                case,
                sessions,
                turns_per_session,
                reopen_each_turn,
                rebuild_after,
                &expected_outputs,
            ),
            other => panic!("unsupported fs in stress fixture: {other}"),
        }
    }
}

#[test]
fn stress_csv_session_registry_keeps_sorted_unique_ids_after_delete_and_create() {
    for row in csv_dicts(include_str!("fixtures/session_registry_stress.csv")) {
        let case = field(&row, "case");
        let root = mem_root("registry-stress");
        let initial_sessions = parse_usize(&row, "initial_sessions");
        let delete_stride = parse_usize(&row, "delete_stride");
        let extra_sessions = parse_usize(&row, "extra_sessions");
        let system = build_mem_system(&root, Vec::new());

        let initial = (0..initial_sessions)
            .map(|_| system.new_session())
            .collect::<Vec<_>>();
        let deleted = if delete_stride == 0 {
            Vec::new()
        } else {
            initial
                .iter()
                .enumerate()
                .filter_map(|(index, session)| {
                    ((index + 1) % delete_stride == 0).then_some(*session)
                })
                .collect::<Vec<_>>()
        };
        for session in &deleted {
            system.delete_session(*session).unwrap();
        }
        let extra = (0..extra_sessions)
            .map(|_| system.new_session())
            .collect::<Vec<_>>();

        let listed = system.list_sessions();
        assert_sorted_unique(&listed, case);
        assert!(
            deleted.iter().all(|session| !listed.contains(session)),
            "case {case}: deleted sessions still listed: {listed:?}"
        );
        assert!(
            extra
                .iter()
                .all(|session| session.0 > initial.last().map_or(0, |id| id.0)),
            "case {case}: extra ids should continue monotonically: {extra:?}"
        );
    }
}

#[test]
fn async_csv_control_storm_on_cloned_controls_finishes_and_accepts_next_submit() {
    for row in csv_dicts(include_str!("fixtures/async_control_storm.csv")) {
        let case = field(&row, "case");
        let root = mem_root("control-storm");
        let interrupts = parse_usize(&row, "interrupts");
        let cancels = parse_usize(&row, "cancels");
        let post_storm_submit = parse_bool(field(&row, "post_storm_submit"));
        YIELDING_HTTP_CALLS.store(0, Ordering::SeqCst);
        YIELDING_HTTP_POLLS.store(0, Ordering::SeqCst);

        let system =
            YieldingAgentSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
        system
            .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
            .unwrap();
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit(Message::text(format!("storm start {case}")))).unwrap();
        let handles = spawn_control_storm(&control, interrupts, cancels);
        for handle in handles {
            let result = handle.join().expect("control worker should not panic");
            assert_control_storm_result(result, session, case);
        }

        let first_events = drain_until_turn_ended(&mut events);
        assert!(
            first_events
                .iter()
                .any(|event| matches!(event, SessionEvent::TurnEnded { .. })),
            "case {case}: first storm turn did not end: {first_events:?}"
        );
        assert!(
            YIELDING_HTTP_POLLS.load(Ordering::SeqCst) > 0,
            "case {case}: yielding HTTP was not exercised"
        );

        if post_storm_submit {
            block_on(control.submit(Message::text(format!("after storm {case}")))).unwrap();
            let second_events = drain_until_turn_ended(&mut events);
            assert!(
                output_fragments(&second_events)
                    .iter()
                    .any(|output| output.starts_with("yielding-call-")),
                "case {case}: second submit should produce output: {second_events:?}"
            );
        }
    }
}

#[derive(Default)]
struct StressScriptHttp;

impl ClawHttp for StressScriptHttp {
    fn post_json<'a>(
        &'a mut self,
        request: &'a HttpJsonRequest<'a>,
        _cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            let text = if is_agent_iteration_request(request.body) {
                stress_outputs()
                    .pop_front()
                    .expect("stress root chat called more times than scripted")
            } else {
                "[]".to_string()
            };
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body: assistant_text(&text),
            })
        })
    }
}

#[derive(Default)]
struct YieldingCountingHttp;

impl ClawHttp for YieldingCountingHttp {
    fn post_json<'a>(
        &'a mut self,
        _request: &'a HttpJsonRequest<'a>,
        cancel: Cancel<'a>,
    ) -> HttpResponseFuture<'a> {
        Box::pin(async move {
            for _ in 0..64 {
                YIELDING_HTTP_POLLS.fetch_add(1, Ordering::SeqCst);
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                yield_once().await;
            }
            if cancel.is_cancelled() {
                return Err(HttpError::Aborted);
            }
            let call = YIELDING_HTTP_CALLS
                .fetch_add(1, Ordering::SeqCst)
                .saturating_add(1);
            Ok(HttpResponse {
                status_code: HttpStatusCode::OK,
                body: assistant_text(&format!("yielding-call-{call}")),
            })
        })
    }
}

fn run_mem_stress_case(
    case: &str,
    sessions: usize,
    turns_per_session: usize,
    reopen_each_turn: bool,
    expected_outputs: &[String],
) {
    let root = mem_root("session-stress");
    MemFs::new();
    install_stress_outputs(expected_outputs);
    let system = MemStressSystem::new::<StdThread, TokioExecutor>(persistence(&root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    drive_stress_case(
        case,
        &system,
        sessions,
        turns_per_session,
        reopen_each_turn,
        expected_outputs,
    );
}

fn run_disk_stress_case(
    case: &str,
    sessions: usize,
    turns_per_session: usize,
    reopen_each_turn: bool,
    rebuild_after: bool,
    expected_outputs: &[String],
) {
    let temp = TempDir::new("claw-agent-stress").unwrap();
    let root = temp.path().to_string_lossy().into_owned();
    install_stress_outputs(expected_outputs);
    let first = build_disk_stress_system(&root);
    let created = drive_stress_case(
        case,
        &first,
        sessions,
        turns_per_session,
        reopen_each_turn,
        expected_outputs,
    );
    assert!(
        DiskFs::exists(&format!("{root}/checkpoint/manifest.json")),
        "case {case}: disk checkpoint manifest missing"
    );

    if rebuild_after {
        drop(first);
        install_stress_outputs(&[]);
        let rebuilt = build_disk_stress_system(&root);
        assert_eq!(rebuilt.list_sessions(), created, "case {case}");
        let next = rebuilt.new_session();
        assert!(
            next.0 > created.last().map_or(0, |id| id.0),
            "case {case}: rebuilt allocator reused ids"
        );
    }
}

fn drive_stress_case<System>(
    case: &str,
    system: &System,
    sessions: usize,
    turns_per_session: usize,
    reopen_each_turn: bool,
    expected_outputs: &[String],
) -> Vec<SessionId>
where
    System: DriveSystem,
{
    let sessions = (0..sessions)
        .map(|_| system.new_session())
        .collect::<Vec<_>>();
    assert_sorted_unique(&sessions, case);

    let mut expected_index = 0usize;
    for session in &sessions {
        if reopen_each_turn {
            for turn in 0..turns_per_session {
                let (control, mut events) = system.open_session(*session);
                block_on(control.submit(Message::text(format!(
                    "{case} session {session} turn {turn}"
                ))))
                .unwrap();
                assert_turn_output(
                    case,
                    &mut events,
                    expected_outputs
                        .get(expected_index)
                        .expect("stress expected output"),
                );
                expected_index = expected_index.saturating_add(1);
                block_on(control.close_session()).unwrap();
                assert_closed(&mut events);
            }
        } else {
            let (control, mut events) = system.open_session(*session);
            for turn in 0..turns_per_session {
                block_on(control.submit(Message::text(format!(
                    "{case} session {session} turn {turn}"
                ))))
                .unwrap();
                assert_turn_output(
                    case,
                    &mut events,
                    expected_outputs
                        .get(expected_index)
                        .expect("stress expected output"),
                );
                expected_index = expected_index.saturating_add(1);
            }
        }
    }
    assert_eq!(
        expected_index,
        expected_outputs.len(),
        "case {case}: not every scripted output was consumed"
    );
    sessions
}

trait DriveSystem {
    fn new_session(&self) -> SessionId;
    fn open_session(&self, session: SessionId) -> (SessionControl, SessionEventStream);
}

impl DriveSystem for MemStressSystem {
    fn new_session(&self) -> SessionId {
        self.new_session()
    }

    fn open_session(&self, session: SessionId) -> (SessionControl, SessionEventStream) {
        self.open_session(session).unwrap()
    }
}

impl DriveSystem for DiskStressSystem {
    fn new_session(&self) -> SessionId {
        self.new_session()
    }

    fn open_session(&self, session: SessionId) -> (SessionControl, SessionEventStream) {
        self.open_session(session).unwrap()
    }
}

fn build_disk_stress_system(root: &str) -> DiskStressSystem {
    let system = DiskStressSystem::new::<StdThread, TokioExecutor>(persistence(root)).unwrap();
    system
        .link_api(llm_config(), claw_agent::ApiUsage::RootAgent, true)
        .unwrap();
    system
}

fn expected_outputs(
    case: &str,
    sessions: usize,
    turns_per_session: usize,
    output_bytes: usize,
) -> Vec<String> {
    let mut outputs = Vec::with_capacity(sessions.saturating_mul(turns_per_session));
    for session in 0..sessions {
        for turn in 0..turns_per_session {
            outputs.push(fixed_width_output(case, session, turn, output_bytes));
        }
    }
    outputs
}

fn fixed_width_output(case: &str, session: usize, turn: usize, output_bytes: usize) -> String {
    let prefix = format!("{case}:{session}:{turn}:");
    if prefix.len() >= output_bytes {
        return prefix;
    }
    let mut output = String::with_capacity(output_bytes);
    output.push_str(&prefix);
    output.extend(std::iter::repeat_n('x', output_bytes - prefix.len()));
    output
}

fn assert_turn_output(case: &str, events: &mut SessionEventStream, expected: &str) {
    let events = drain_until_turn_ended(events);
    assert!(
        events
            .iter()
            .any(|event| matches!(event, SessionEvent::TurnEnded { .. })),
        "case {case}: missing TurnEnded: {events:?}"
    );
    assert_eq!(
        output_fragments(&events),
        vec![expected.to_owned()],
        "case {case}"
    );
}

fn spawn_control_storm(
    control: &claw_agent::SessionControl,
    interrupts: usize,
    cancels: usize,
) -> Vec<thread::JoinHandle<Result<(), SessionControlError>>> {
    let mut handles = Vec::with_capacity(interrupts.saturating_add(cancels));
    for _ in 0..interrupts {
        let control = control.clone();
        handles.push(thread::spawn(move || block_on(control.interrupt())));
    }
    for _ in 0..cancels {
        let control = control.clone();
        handles.push(thread::spawn(move || block_on(control.cancel())));
    }
    handles
}

fn assert_control_storm_result(
    result: Result<(), SessionControlError>,
    session: SessionId,
    case: &str,
) {
    match result {
        Ok(()) => {}
        Err(SessionControlError::SessionClosed(closed)) if closed == session => {}
        other => panic!("case {case}: unexpected control storm result {other:?}"),
    }
}

fn assert_sorted_unique(sessions: &[SessionId], case: &str) {
    let mut sorted = sessions.to_vec();
    sorted.sort_by_key(|session| session.0);
    assert_eq!(sessions, sorted.as_slice(), "case {case}: ids not sorted");
    let unique = sessions.iter().copied().collect::<HashSet<_>>();
    assert_eq!(
        unique.len(),
        sessions.len(),
        "case {case}: duplicate sessions: {sessions:?}"
    );
}

fn assert_closed(events: &mut SessionEventStream) {
    let events = drain_until_closed(events);
    assert_eq!(events.last(), Some(&SessionEvent::Closed));
}

fn drain_until_closed(events: &mut SessionEventStream) -> Vec<SessionEvent> {
    block_on(async move {
        let mut collected = Vec::new();
        while let Some(event) = events.next().await {
            let closed = event == SessionEvent::Closed;
            collected.push(event);
            if closed {
                break;
            }
        }
        collected
    })
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

fn install_stress_outputs(outputs: &[String]) {
    *stress_outputs() = outputs.iter().cloned().collect();
}

fn stress_outputs() -> MutexGuard<'static, VecDeque<String>> {
    STRESS_OUTPUTS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn is_agent_iteration_request(body: &str) -> bool {
    let Ok(value) = serde_json::from_str::<serde_json::Value>(body) else {
        return false;
    };
    value.get("tools").is_some() && value.get("response_format").is_none()
}

async fn yield_once() {
    struct YieldOnce(bool);

    impl std::future::Future for YieldOnce {
        type Output = ();

        fn poll(
            mut self: std::pin::Pin<&mut Self>,
            context: &mut std::task::Context<'_>,
        ) -> std::task::Poll<()> {
            if self.0 {
                std::task::Poll::Ready(())
            } else {
                self.0 = true;
                context.waker().wake_by_ref();
                std::task::Poll::Pending
            }
        }
    }

    YieldOnce(false).await;
}

fn parse_usize(row: &BTreeMap<String, String>, field_name: &str) -> usize {
    field(row, field_name).parse::<usize>().unwrap()
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
