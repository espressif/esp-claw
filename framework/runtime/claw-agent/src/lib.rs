//! `claw_agent` wires tools and sessions to the core orchestrator.
//!
//! `AgentSystem` owns sessions and exposes session connections. Transport
//! routing, channel inbound/outbound conversion, and reply destinations live in
//! adapter crates above this layer.

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::ClawApiConfig;
pub use claw_core::{
    IterationId, OpenSessionError, SessionControl, SessionControlError, SessionEvent,
    SessionEventStream, SessionId, TurnCause,
};
use claw_core::{Orchestrator, OrchestratorBuildError};
use claw_interface::{ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, FsError};
#[cfg(feature = "host-backends")]
use claw_interface::{DiskFs, RealHttp, StdThread, TokioExecutor, TokioTimer};
use claw_tool::{ToolRegistry, ToolRegistryError};

#[cfg(feature = "host-backends")]
pub type HostAgentSystem = AgentSystem<DiskFs, RealHttp, TokioTimer>;

pub type AgentResult<T> = Result<T, AgentError>;

/// Explicit storage root for an [`AgentSystem`], plus the skill roots the agent
/// factory scans to populate every agent's skill catalog.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentPersistenceConfig {
    pub persistence_root: String,
    /// Skill roots in priority order (e.g. DATA before SYSTEM). Empty means no
    /// filesystem skills are loaded.
    pub skill_roots: Vec<String>,
}

/// What can go wrong while building or driving an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// Building the core orchestrator failed.
    #[error(transparent)]
    Orchestrator(#[from] OrchestratorBuildError),
    /// The tool registry failed.
    #[error(transparent)]
    Tool(#[from] ToolRegistryError),
    /// Opening a session event stream failed.
    #[error(transparent)]
    OpenSession(#[from] OpenSessionError),
    /// The scratch storage root could not be cleared before startup.
    #[error("failed to clear agent storage at {path}: {source}")]
    StorageClear {
        path: String,
        #[source]
        source: FsError,
    },
}

/// A ready-to-drive agent runtime.
///
/// The `Filesystem`/`Http`/`Timer` backends select which concrete filesystem,
/// HTTP, and timer the orchestrator's drive worker uses; they are only needed at
/// construction, so they are held as a marker (the built [`Orchestrator`] handle
/// is backend-erased and `Send + Sync`).
pub struct AgentSystem<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + Clone + Default + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    tools: Arc<ToolRegistry>,
    orchestrator: Orchestrator,
    _marker: PhantomData<fn() -> (Filesystem, Http, Timer)>,
}

impl<Filesystem, Http, Timer> AgentSystem<Filesystem, Http, Timer>
where
    Filesystem: ClawFs + Clone + Default + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build a fully injectable agent system, spawning the orchestrator's drive
    /// worker via the [`ClawThread`] policy `Thread` (`StdThread` on host,
    /// `EspIdfThread` on device) and driving its `!Send` engine with the injected
    /// [`ClawExecutor`] `Executor` (`TokioExecutor` on host,
    /// `EspIdfExecutor` on device).
    /// Both are zero-sized policies selected purely by type parameter, like the
    /// `Filesystem`/`Http`/`Timer` backends.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when storage cleanup or orchestrator construction fails.
    pub fn new<Thread, Executor>(
        llm_config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> AgentResult<Self>
    where
        Thread: ClawThread,
        Executor: ClawExecutor + 'static,
    {
        let tools = Arc::new(ToolRegistry::new());
        let orchestrator = Orchestrator::new::<Filesystem, Http, Timer, Thread, Executor>(
            Arc::clone(&tools),
            llm_config,
            persistence.persistence_root,
            persistence.skill_roots,
        )?;

        Ok(Self {
            tools,
            orchestrator,
            _marker: PhantomData,
        })
    }

    /// Tool registry used by this system.
    pub fn tool_registry(&self) -> &ToolRegistry {
        &self.tools
    }

    /// Start every registered tool.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when the tool registry fails to start.
    pub fn start_all(&self) -> AgentResult<()> {
        self.tools.start_all()?;
        Ok(())
    }

    /// Stop every registered tool.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when the tool registry fails to stop.
    pub fn stop_all(&self) -> AgentResult<()> {
        self.tools.stop_all()?;
        Ok(())
    }

    /// Open a live session's command and event halves.
    ///
    /// The returned [`SessionControl`] accepts user inputs and session control
    /// commands; the returned [`SessionEventStream`] is the only user-visible
    /// event outlet for the session.
    ///
    /// # Errors
    ///
    /// Returns [`OpenSessionError`] when the session is missing, already open, or
    /// the orchestrator worker is stopped.
    pub fn open_session(
        &self,
        session: SessionId,
    ) -> AgentResult<(SessionControl, SessionEventStream)> {
        Ok(self.orchestrator.open_session(session)?)
    }

    /// Create a fresh isolated conversation session.
    pub fn new_session(&self) -> SessionId {
        self.orchestrator.session_create()
    }

    /// Return the live conversation sessions.
    pub fn list_sessions(&self) -> Vec<SessionId> {
        self.orchestrator.session_list()
    }
}

#[cfg(feature = "host-backends")]
impl AgentSystem<DiskFs, RealHttp, TokioTimer> {
    /// Build a host-target agent system backed by disk memory and live HTTP.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when construction fails.
    pub fn on_disk(llm: ClawApiConfig, persistence: AgentPersistenceConfig) -> AgentResult<Self> {
        Self::new::<StdThread, TokioExecutor>(llm, persistence)
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use core::future::Future;
    use core::pin::Pin;
    use core::task::{Context, Poll};

    use futures_lite::future::block_on;
    use futures_lite::StreamExt;

    use claw_api::{BackendKind, ClawApiConfig};
    use claw_interface::{
        BlockingHttpAdapter, Cancel, ClawHttp, HttpError, HttpJsonRequest, HttpResponseFuture,
        ImmediateTimer, MemFs, SharedScriptHttp, StdThread, TokioExecutor,
    };
    use serde_json::json;

    use super::*;

    type TestSystem = AgentSystem<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;
    type SlowTestSystem = AgentSystem<MemFs, SlowScriptHttp, ImmediateTimer>;

    #[derive(Default)]
    struct SlowScriptHttp;

    impl ClawHttp for SlowScriptHttp {
        fn post_json<'a>(
            &'a mut self,
            request: &'a HttpJsonRequest<'a>,
            cancel: Cancel<'a>,
        ) -> HttpResponseFuture<'a> {
            Box::pin(async move {
                YieldTimes::new(16).await;
                if cancel.is_cancelled() {
                    return Err(HttpError::Aborted);
                }
                let mut inner = BlockingHttpAdapter::new(SharedScriptHttp::default());
                inner.post_json(request, cancel).await
            })
        }
    }

    struct YieldTimes {
        remaining: u32,
    }

    impl YieldTimes {
        const fn new(remaining: u32) -> Self {
            Self { remaining }
        }
    }

    impl Future for YieldTimes {
        type Output = ();

        fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
            if self.remaining == 0 {
                Poll::Ready(())
            } else {
                self.remaining -= 1;
                context.waker().wake_by_ref();
                Poll::Pending
            }
        }
    }

    #[test]
    fn clear_storage_tree_removes_nested_files() {
        let fs = MemFs::default();
        fs.write_atomic("/agent/sessions/roots/conversation-1.jsonl", b"root")
            .unwrap();
        fs.write_atomic("/agent/sessions/agents/conversation-2.jsonl", b"sub")
            .unwrap();
        fs.write_atomic("/agent/profile/user.md", b"profile")
            .unwrap();

        clear_storage_tree(&fs, "/agent").unwrap();

        assert!(!fs.exists("/agent/sessions/roots/conversation-1.jsonl"));
        assert!(!fs.exists("/agent/sessions/agents/conversation-2.jsonl"));
        assert!(!fs.exists("/agent/profile/user.md"));
    }

    #[test]
    fn list_sessions_returns_session_ids() {
        let _script = SharedScriptHttp::serialize();
        let system = test_system(vec![]);
        let session = system.new_session();

        let sessions = system.list_sessions();
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions[0], session);
    }

    fn drain_until_turn_ended(events: &mut SessionEventStream) -> Vec<SessionEvent> {
        block_on(async move {
            let mut collected = Vec::new();
            while let Some(event) = events.next().await {
                let ended = event == SessionEvent::TurnEnded;
                collected.push(event);
                if ended {
                    break;
                }
            }
            collected
        })
    }

    #[test]
    fn session_streams_root_reply_as_output() {
        let _script = SharedScriptHttp::serialize();
        let system = test_system(vec![assistant_text("hello there")]);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(control.submit("say hi")).unwrap();
        let events = drain_until_turn_ended(&mut events);

        assert_eq!(
            events.first(),
            Some(&SessionEvent::TurnStarted {
                cause: TurnCause::UserSubmit
            })
        );
        assert_eq!(events.last(), Some(&SessionEvent::TurnEnded));
        let outputs: Vec<&str> = events
            .iter()
            .filter_map(|event| match event {
                SessionEvent::Output { text } => Some(text.as_str()),
                _ => None,
            })
            .collect();
        assert_eq!(outputs, vec!["hello there"]);
    }

    #[test]
    fn open_unknown_session_returns_error() {
        let _script = SharedScriptHttp::serialize();
        let system = test_system(vec![]);
        assert!(matches!(
            system.open_session(SessionId(9)),
            Err(AgentError::OpenSession(OpenSessionError::SessionNotFound(
                SessionId(9)
            )))
        ));
    }

    #[test]
    fn second_submit_returns_busy_until_current_turn_ends() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("first"), assistant_text("second")]);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(async {
            control.submit("first").await.unwrap();
            assert_eq!(
                control.submit("second").await,
                Err(SessionControlError::Busy(session))
            );
        });
        let first_events = drain_until_turn_ended(&mut events);

        assert!(first_events
            .iter()
            .any(|event| matches!(event, SessionEvent::Output { text } if text == "first")));
        block_on(control.submit("second")).unwrap();
        let second_events = drain_until_turn_ended(&mut events);
        assert!(second_events
            .iter()
            .any(|event| matches!(event, SessionEvent::Output { text } if text == "second")));
    }

    #[test]
    fn session_control_methods_are_idempotent() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("cancelled")]);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(async {
            control.submit("cancel me").await.unwrap();
            control.interrupt().await.unwrap();
            control.interrupt().await.unwrap();
            control.cancel().await.unwrap();
            control.cancel().await.unwrap();
        });

        let events = drain_until_turn_ended(&mut events);
        assert!(matches!(
            events.first(),
            Some(SessionEvent::TurnStarted {
                cause: TurnCause::UserSubmit
            })
        ));
        assert_eq!(events.last(), Some(&SessionEvent::TurnEnded));
    }

    #[test]
    fn close_session_cancels_active_work_and_closes_events() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("should not surface")]);
        let session = system.new_session();
        let (control, mut events) = system.open_session(session).unwrap();

        block_on(async {
            control.submit("delete me").await.unwrap();
            control.close_session().await.unwrap();
        });
        let events = block_on(async {
            let mut collected = Vec::new();
            while let Some(event) = events.next().await {
                let closed = event == SessionEvent::Closed;
                collected.push(event);
                if closed {
                    break;
                }
            }
            collected
        });

        assert_eq!(events.last(), Some(&SessionEvent::Closed));
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, SessionEvent::Output { .. })),
            "deleted stream should be cancelled without output: {events:?}"
        );
        assert!(
            block_on(control.submit("after close")).is_err(),
            "closed session should reject new submits"
        );
    }

    fn test_system(bodies: Vec<String>) -> TestSystem {
        install_script(bodies);
        TestSystem::new::<StdThread, TokioExecutor>(
            llm_config(),
            AgentPersistenceConfig {
                persistence_root: "/mem".to_string(),
                skill_roots: Vec::new(),
            },
        )
        .unwrap()
    }

    fn slow_test_system(bodies: Vec<String>) -> SlowTestSystem {
        install_script(bodies);
        SlowTestSystem::new::<StdThread, TokioExecutor>(
            llm_config(),
            AgentPersistenceConfig {
                persistence_root: "/mem".to_string(),
                skill_roots: Vec::new(),
            },
        )
        .unwrap()
    }

    fn install_script(bodies: Vec<String>) {
        let mut script = Vec::with_capacity(bodies.len().saturating_add(1));
        if !bodies.is_empty() {
            script.push(assistant_text("[]"));
        }
        for body in bodies {
            script.push(body);
        }
        SharedScriptHttp::install(script);
    }

    fn llm_config() -> ClawApiConfig {
        ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        )
    }

    fn assistant_text(text: &str) -> String {
        json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
    }
}
