//! `claw_agent` wires tools and sessions to the core orchestrator.
//!
//! `AgentSystem` owns sessions and exposes direct session submission. Transport
//! routing, channel inbound/outbound conversion, and reply destinations live in
//! adapter crates above this layer.

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::ClawApiConfig;
pub use claw_core::{
    AgentEvent, IterationId, SessionId, SubmitControl, SubmitControlError,
    SubmitStream as AgentEventStream,
};
use claw_core::{DeliverError, Orchestrator, OrchestratorBuildError, SessionError, SubmitStream};
use claw_interface::{ClawExecutor, ClawFs, ClawHttp, ClawThread, ClawTimer, FsError};
#[cfg(feature = "host-backends")]
use claw_interface::{DiskFs, RealHttp, StdThread, TokioExecutor, TokioTimer};
use claw_tool::{SharedContext, ToolRegistry, ToolRegistryError};

#[cfg(feature = "host-backends")]
pub type HostAgentSystem = AgentSystem<DiskFs, RealHttp, TokioTimer>;

pub type AgentResult<T> = Result<T, AgentError>;

/// Explicit storage root for an [`AgentSystem`], plus the skill roots the agent
/// factory scans to populate every agent's skill catalog.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentPersistenceConfig {
    dir: String,
    /// Skill roots in priority order (e.g. DATA before SYSTEM). Empty means no
    /// filesystem skills are loaded.
    skill_roots: Vec<String>,
}

impl AgentPersistenceConfig {
    /// Build storage config from the required root directory. No skill roots are
    /// attached; use [`AgentPersistenceConfig::with_skill_roots`] to add them.
    pub fn new(dir: &str) -> Self {
        Self {
            dir: dir.to_string(),
            skill_roots: Vec::new(),
        }
    }

    /// Attach the skill roots the factory scans, in priority order.
    #[must_use]
    pub fn with_skill_roots(mut self, skill_roots: Vec<String>) -> Self {
        self.skill_roots = skill_roots;
        self
    }
}

/// What can go wrong while building or driving an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// No storage directory was provided.
    #[error("agent storage directory is required")]
    MissingPersistenceDir,
    /// The dedicated extraction LLM client failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(String),
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(String),
    /// The tool registry failed.
    #[error(transparent)]
    Tool(#[from] ToolRegistryError),
    /// Core delivery failed.
    #[error(transparent)]
    Deliver(#[from] DeliverError),
    /// Session operation failed.
    #[error(transparent)]
    Session(#[from] SessionError),
    /// The scratch storage root could not be cleared before startup.
    #[error("failed to clear agent storage at {path}: {source}")]
    StorageClear {
        path: String,
        #[source]
        source: FsError,
    },
    /// The orchestrator's drive worker could not be started.
    #[error("failed to start the orchestrator worker: {0}")]
    Worker(String),
}

impl From<OrchestratorBuildError> for AgentError {
    fn from(error: OrchestratorBuildError) -> Self {
        match error {
            OrchestratorBuildError::MissingPersistenceDir => Self::MissingPersistenceDir,
            OrchestratorBuildError::ExtractionLlm(message) => Self::ExtractionLlm(message),
            OrchestratorBuildError::LongTermInit(message) => Self::LongTermInit(message),
            OrchestratorBuildError::Worker(message) => Self::Worker(message),
        }
    }
}

/// A ready-to-drive agent runtime.
///
/// The `F`/`H`/`Timer` backends select which concrete filesystem, HTTP, and timer
/// the orchestrator's drive worker uses; they are only needed at construction, so
/// they are held as a marker (the built [`Orchestrator`] handle is backend-erased
/// and `Send + Sync`).
pub struct AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    tools: Arc<ToolRegistry>,
    orchestrator: Orchestrator,
    _marker: PhantomData<fn() -> (F, H, Timer)>,
}

impl<F, H, Timer> AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build a fully injectable agent system, spawning the orchestrator's drive
    /// worker via the [`ClawThread`] policy `T` (`StdThread` on host,
    /// `EspIdfThread` on device) and driving its `!Send` engine with the injected
    /// [`ClawExecutor`] `E` (`TokioExecutor` on host, `EspIdfExecutor` on device).
    /// Both are zero-sized policies selected purely by type parameter, like the
    /// `F`/`H`/`Timer` backends.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when storage cleanup or orchestrator construction fails.
    pub fn new<T, E>(
        llm_config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> AgentResult<Self>
    where
        T: ClawThread,
        E: ClawExecutor + 'static,
    {
        let persistence_dir = persistence.dir;
        if persistence_dir.trim().is_empty() {
            return Err(AgentError::MissingPersistenceDir);
        }
        let storage = F::default();
        clear_storage_tree(&storage, &persistence_dir)?;

        let tools = Arc::new(ToolRegistry::new());
        let orchestrator = Orchestrator::new::<F, H, Timer, T, E>(
            Arc::clone(&tools),
            llm_config,
            &persistence_dir,
            &persistence.skill_roots,
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

    /// Submit one text input to an explicitly chosen session and stream its turn.
    ///
    /// Returns immediately with a [`SubmitStream`]: an async stream of
    /// [`AgentEvent`]s. The turn runs as the caller drains the stream, which ends
    /// when the turn finishes. A submit to an unknown session, or to a session
    /// that already has an active submission, surfaces as a single
    /// [`AgentEvent::Error`] before the stream ends, so this method is infallible
    /// at the call boundary.
    pub fn submit(&self, session: SessionId, text: impl Into<String>) -> SubmitStream {
        self.orchestrator.submit(session, text.into(), None)
    }

    /// Like [`submit`](Self::submit) but installs a type-erased per-submission
    /// `context` (via [`claw_tool::current_context`]) for the duration of this
    /// turn's drive, so tool handlers can read caller-supplied request metadata.
    pub fn submit_with_context(
        &self,
        session: SessionId,
        text: impl Into<String>,
        context: Option<SharedContext>,
    ) -> SubmitStream {
        self.orchestrator.submit(session, text.into(), context)
    }

    /// Create a fresh isolated conversation session.
    pub fn new_session(&self) -> SessionId {
        self.orchestrator.session_create()
    }

    /// Return the live conversation sessions.
    pub fn list_sessions(&self) -> Vec<SessionId> {
        self.orchestrator.session_list()
    }

    /// Delete a session.
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::NotFound`] when `session` is not live.
    pub fn delete_session(&self, session: SessionId) -> AgentResult<()> {
        self.orchestrator.session_delete(session)?;
        Ok(())
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

fn join_storage_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{child}");
    }
    let parent = parent.trim_end_matches('/');
    if parent.is_empty() {
        child.to_string()
    } else {
        format!("{parent}/{child}")
    }
}

fn clear_storage_tree<F: ClawFs>(fs: &F, path: &str) -> AgentResult<()> {
    match fs.list_dir(path) {
        Ok(entries) => {
            for entry in entries {
                let child = join_storage_path(path, &entry);
                clear_storage_tree(fs, &child)?;
            }
            let _ = fs.remove(path);
            Ok(())
        }
        Err(FsError::NotFound) => Ok(()),
        Err(source) => Err(AgentError::StorageClear {
            path: path.to_string(),
            source,
        }),
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

    fn drain(stream: SubmitStream) -> Vec<AgentEvent> {
        block_on(stream.collect())
    }

    fn drain_until_turn_ended(mut stream: SubmitStream) -> Vec<AgentEvent> {
        block_on(async move {
            let mut events = Vec::new();
            while let Some(event) = stream.next().await {
                let ended = event == AgentEvent::TurnEnded;
                events.push(event);
                if ended {
                    break;
                }
            }
            events
        })
    }

    #[test]
    fn submit_streams_root_reply_as_output() {
        let _script = SharedScriptHttp::serialize();
        let system = test_system(vec![assistant_text("hello there")]);
        let session = system.new_session();

        let events = drain(system.submit(session, "say hi".to_string()));

        assert_eq!(events.first(), Some(&AgentEvent::TurnStarted));
        assert_eq!(events.last(), Some(&AgentEvent::TurnEnded));
        let outputs: Vec<&str> = events
            .iter()
            .filter_map(|event| match event {
                AgentEvent::Output { text } => Some(text.as_str()),
                _ => None,
            })
            .collect();
        assert_eq!(outputs, vec!["hello there"]);
    }

    #[test]
    fn submit_unknown_session_streams_error() {
        let _script = SharedScriptHttp::serialize();
        let system = test_system(vec![]);
        let events = drain(system.submit(SessionId(9), "x".to_string()));
        assert!(matches!(
            events.as_slice(),
            [AgentEvent::Error { message }] if message.contains("session-9")
        ));
    }

    #[test]
    fn concurrent_submit_to_same_session_streams_error() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("first")]);
        let session = system.new_session();

        let first = system.submit(session, "first".to_string());
        let second_events = drain(system.submit(session, "second".to_string()));
        let first_events = drain(first);

        assert!(matches!(
            second_events.as_slice(),
            [AgentEvent::Error { message }] if message.contains("active submission")
        ));
        assert!(first_events
            .iter()
            .any(|event| matches!(event, AgentEvent::Output { text } if text == "first")));
    }

    #[test]
    fn submit_stream_control_methods_are_idempotent() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("cancelled")]);
        let session = system.new_session();
        let stream = system.submit(session, "cancel me".to_string());

        assert!(stream.interrupt().is_ok());
        assert!(stream.interrupt().is_ok());
        assert!(stream.cancel().is_ok());
        assert!(stream.cancel().is_ok());

        let events = drain(stream);
        assert_eq!(events.first(), Some(&AgentEvent::TurnStarted));
        assert_eq!(events.last(), Some(&AgentEvent::TurnEnded));
    }

    #[test]
    fn delete_session_cancels_active_stream() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("should not surface")]);
        let session = system.new_session();
        let stream = system.submit(session, "delete me".to_string());

        system.delete_session(session).unwrap();
        let after_delete = drain(system.submit(session, "after delete".to_string()));
        let events = drain(stream);

        assert!(matches!(
            after_delete.as_slice(),
            [AgentEvent::Error { message }] if message.contains("session-")
        ));
        assert_eq!(events.first(), Some(&AgentEvent::TurnStarted));
        assert_eq!(events.last(), Some(&AgentEvent::TurnEnded));
        assert!(
            !events
                .iter()
                .any(|event| matches!(event, AgentEvent::Output { .. })),
            "deleted stream should be cancelled without output: {events:?}"
        );
    }

    #[test]
    fn stale_stream_control_does_not_cancel_new_submission() {
        let _script = SharedScriptHttp::serialize();
        let system = slow_test_system(vec![assistant_text("first"), assistant_text("second")]);
        let session = system.new_session();

        let first = system.submit(session, "first".to_string());
        let stale_control = first.control();
        let first_events = drain_until_turn_ended(first);
        assert!(first_events
            .iter()
            .any(|event| matches!(event, AgentEvent::Output { text } if text == "first")));

        let second = system.submit(session, "second".to_string());
        assert!(stale_control.cancel().is_ok());
        let second_events = drain(second);

        assert!(
            second_events
                .iter()
                .any(|event| matches!(event, AgentEvent::Output { text } if text == "second")),
            "second events: {second_events:?}"
        );
    }

    fn test_system(bodies: Vec<String>) -> TestSystem {
        install_script(bodies);
        TestSystem::new::<StdThread, TokioExecutor>(
            llm_config(),
            AgentPersistenceConfig::new("/mem"),
        )
        .unwrap()
    }

    fn slow_test_system(bodies: Vec<String>) -> SlowTestSystem {
        install_script(bodies);
        SlowTestSystem::new::<StdThread, TokioExecutor>(
            llm_config(),
            AgentPersistenceConfig::new("/mem"),
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
