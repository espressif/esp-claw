//! `claw_agent` wires channels and tools to the core orchestrator.
//!
//! `AgentSystem` owns sessions and remembers where each session should send
//! replies. It does not auto-bind channel chats to sessions; embedding layers
//! choose the session and call `submit_channel`.

use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

use claw_api::ClawApiConfig;
use claw_channel::{
    ChannelInbound, ChannelOutbound, ChannelRegistry, ChannelRegistryError, ChannelSink,
    ChannelTarget, ChannelTargetOwned,
};
use claw_core::{
    DeliverError, DeliveryKind, DriveOutput, Orchestrator, OrchestratorBuildError, SessionError,
    SessionId,
};
use claw_interface::{ClawFs, ClawHttp, ClawTimer, FsError};
#[cfg(feature = "host-backends")]
use claw_interface::{DiskFs, RealHttp, TokioTimer};
use claw_tool::{ToolRegistry, ToolRegistryError};

#[cfg(feature = "host-backends")]
pub type HostAgentSystem = AgentSystem<DiskFs, RealHttp, TokioTimer>;

pub type AgentResult<T> = Result<T, AgentError>;

/// Explicit storage root for an [`AgentSystem`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentPersistenceConfig {
    dir: String,
}

impl AgentPersistenceConfig {
    /// Build storage config from the required root directory.
    pub fn new(dir: &str) -> Self {
        Self {
            dir: dir.to_string(),
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct SessionTarget {
    target: ChannelTargetOwned,
    correlation_id: Option<String>,
}

/// One live conversation session as exposed by [`AgentSystem`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionRecord {
    pub id: SessionId,
    pub target: Option<ChannelTargetOwned>,
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
    /// A channel input did not carry a usable target.
    #[error("invalid channel input")]
    InvalidChannelInput,
    /// A reply was produced before this session had a target.
    #[error("no reply target for {0}")]
    NoReplyTarget(SessionId),
    /// The tool registry failed.
    #[error(transparent)]
    Tool(#[from] ToolRegistryError),
    /// A channel operation failed.
    #[error(transparent)]
    Channel(#[from] ChannelRegistryError),
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
}

impl From<OrchestratorBuildError> for AgentError {
    fn from(error: OrchestratorBuildError) -> Self {
        match error {
            OrchestratorBuildError::MissingPersistenceDir => Self::MissingPersistenceDir,
            OrchestratorBuildError::ExtractionLlm(message) => Self::ExtractionLlm(message),
            OrchestratorBuildError::LongTermInit(message) => Self::LongTermInit(message),
        }
    }
}

/// A ready-to-drive agent runtime.
pub struct AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    tools: Arc<ToolRegistry>,
    channels: Arc<ChannelRegistry>,
    orchestrator: Arc<Orchestrator<F, H, Timer>>,
    session_targets: Arc<Mutex<HashMap<SessionId, SessionTarget>>>,
}

impl<F, H, Timer> Clone for AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn clone(&self) -> Self {
        Self {
            tools: Arc::clone(&self.tools),
            channels: Arc::clone(&self.channels),
            orchestrator: Arc::clone(&self.orchestrator),
            session_targets: Arc::clone(&self.session_targets),
        }
    }
}

impl<F, H, Timer> AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build a fully injectable agent system.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when storage cleanup or orchestrator construction fails.
    pub fn new(
        llm_config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> AgentResult<Self> {
        let persistence_dir = persistence.dir;
        if persistence_dir.trim().is_empty() {
            return Err(AgentError::MissingPersistenceDir);
        }
        let storage = F::default();
        clear_storage_tree(&storage, &persistence_dir)?;

        let tools = Arc::new(ToolRegistry::new());
        let channels = Arc::new(ChannelRegistry::new(ChannelSink::default()));
        let orchestrator = Arc::new(Orchestrator::<F, H, Timer>::new(
            Arc::clone(&tools),
            llm_config,
            &persistence_dir,
        )?);

        Ok(Self {
            tools,
            channels,
            orchestrator,
            session_targets: Arc::new(Mutex::new(HashMap::new())),
        })
    }

    /// Tool registry used by this system.
    pub fn tool_registry(&self) -> &ToolRegistry {
        &self.tools
    }

    /// Channel registry used by this system.
    pub fn channel_registry(&self) -> &ChannelRegistry {
        &self.channels
    }

    /// Start every registered tool and stopped channel.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when any registry fails to start.
    pub fn start_all(&self) -> AgentResult<()> {
        self.tools.start_all()?;
        self.channels.start_all()?;
        Ok(())
    }

    /// Stop channels, then tools.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when any registry fails to stop.
    pub fn stop_all(&self) -> AgentResult<()> {
        self.channels.stop_all()?;
        self.tools.stop_all()?;
        Ok(())
    }

    /// Submit one channel input to an explicitly chosen session.
    ///
    /// Channel input defaults to [`DeliveryKind::Interrupt`].
    ///
    /// # Errors
    ///
    /// Returns [`AgentError`] when the session, input, delivery, or outbound send fails.
    pub async fn submit_channel(
        &self,
        session: SessionId,
        input: ChannelInbound,
    ) -> AgentResult<()> {
        self.validate_session(session)?;
        validate_channel_input(&input)?;
        self.remember_target(session, &input);

        let Some(text) = session_text(&input) else {
            return Ok(());
        };

        let output = self
            .orchestrator
            .submit(session, text, DeliveryKind::Interrupt)
            .await?;
        self.surface_output(output)
    }

    /// Create a fresh isolated conversation session.
    pub fn new_session(&self) -> SessionId {
        self.orchestrator.session_create()
    }

    /// Return the live conversation sessions.
    pub fn list_sessions(&self) -> Vec<SessionRecord> {
        let targets = self.session_targets();
        self.orchestrator
            .session_list()
            .into_iter()
            .map(|id| SessionRecord {
                id,
                target: targets.get(&id).map(|target| target.target.clone()),
            })
            .collect()
    }

    /// Delete a session and forget its channel target.
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::NotFound`] when `session` is not live.
    pub fn delete_session(&self, session: SessionId) -> AgentResult<()> {
        self.orchestrator.session_delete(session)?;
        self.session_targets().remove(&session);
        Ok(())
    }

    fn validate_session(&self, session: SessionId) -> AgentResult<()> {
        if self.orchestrator.session_list().contains(&session) {
            Ok(())
        } else {
            Err(SessionError::NotFound(session).into())
        }
    }

    fn remember_target(&self, session: SessionId, input: &ChannelInbound) {
        let target = input.target.clone().unwrap_or_else(|| ChannelTargetOwned {
            channel: input.channel.clone(),
            chat_id: input.chat_id.clone(),
        });
        let correlation_id = input
            .correlation_id
            .clone()
            .or_else(|| input.message_id.clone());
        self.session_targets().insert(
            session,
            SessionTarget {
                target,
                correlation_id,
            },
        );
    }

    fn surface_output(&self, output: DriveOutput) -> AgentResult<()> {
        for reply in output.replies {
            let route = self
                .session_targets()
                .get(&reply.session)
                .cloned()
                .ok_or(AgentError::NoReplyTarget(reply.session))?;
            self.channels.send(ChannelOutbound {
                target: ChannelTarget {
                    channel: &route.target.channel,
                    chat_id: &route.target.chat_id,
                },
                text: Some(&reply.text),
                attachments: &[],
                message_id: None,
                correlation_id: route.correlation_id.as_deref(),
                payload_json: None,
            })?;
        }
        Ok(())
    }

    fn session_targets(&self) -> MutexGuard<'_, HashMap<SessionId, SessionTarget>> {
        self.session_targets
            .lock()
            .unwrap_or_else(|poison| poison.into_inner())
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
        Self::new(llm, persistence)
    }
}

fn validate_channel_input(input: &ChannelInbound) -> AgentResult<()> {
    if input.channel.trim().is_empty() || input.chat_id.trim().is_empty() {
        return Err(AgentError::InvalidChannelInput);
    }
    if let Some(target) = &input.target {
        if target.channel.trim().is_empty() || target.chat_id.trim().is_empty() {
            return Err(AgentError::InvalidChannelInput);
        }
    }
    Ok(())
}

fn session_text(input: &ChannelInbound) -> Option<String> {
    let text = input.text.as_ref()?;
    if text.trim().is_empty() {
        None
    } else {
        Some(text.clone())
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
    use core::task::Context;
    use std::sync::{Arc, Mutex};
    use std::task::{Wake, Waker};

    use claw_api::{BackendKind, ClawApiConfig};
    use claw_channel::{
        Channel, ChannelHandler, ChannelInbound, ChannelOutbound, ChannelResult, ChannelRuntime,
        ChannelSink, ChannelTargetOwned,
    };
    use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp};
    use serde_json::json;

    use super::*;

    type TestSystem = AgentSystem<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>;

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let std::task::Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
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
    fn list_sessions_projects_targets() {
        let system = test_system(vec![], Arc::new(Mutex::new(Vec::new())));
        let session = system.new_session();
        block_on(system.submit_channel(session, inbound(None))).unwrap();

        let sessions = system.list_sessions();
        assert_eq!(sessions.len(), 1);
        assert_eq!(sessions[0].id, session);
        assert_eq!(
            sessions[0].target.as_ref(),
            Some(&ChannelTargetOwned {
                channel: "web".into(),
                chat_id: "chat".into(),
            })
        );
    }

    #[test]
    fn submit_channel_routes_root_reply_to_target() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let system = test_system(vec![assistant_text("hello there")], Arc::clone(&sent));
        let session = system.new_session();

        block_on(system.submit_channel(session, inbound(Some("say hi")))).unwrap();

        assert_eq!(sent.lock().unwrap().as_slice(), ["hello there"]);
    }

    #[test]
    fn submit_channel_requires_existing_session() {
        let sent = Arc::new(Mutex::new(Vec::new()));
        let system = test_system(vec![], sent);
        let error = block_on(system.submit_channel(SessionId(9), inbound(Some("x")))).unwrap_err();
        assert!(
            matches!(error, AgentError::Session(SessionError::NotFound(id)) if id == SessionId(9))
        );
    }

    fn test_system(bodies: Vec<String>, sent: Arc<Mutex<Vec<String>>>) -> TestSystem {
        let mut script = Vec::with_capacity(bodies.len().saturating_mul(2));
        for body in bodies {
            script.push(assistant_text("[]"));
            script.push(body);
        }
        SharedScriptHttp::install(script);

        let system = TestSystem::new(llm_config(), AgentPersistenceConfig::new("/mem")).unwrap();
        system
            .channel_registry()
            .register(Channel::from_handler(TestChannel { sent }))
            .unwrap();
        system
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

    fn inbound(text: Option<&str>) -> ChannelInbound {
        ChannelInbound {
            channel: "web".into(),
            chat_id: "chat".into(),
            text: text.map(str::to_owned),
            attachments: Vec::new(),
            sender_id: None,
            message_id: Some("m1".into()),
            correlation_id: None,
            timestamp_ms: None,
            target: None,
            content_type: None,
            payload_json: None,
        }
    }

    struct TestChannel {
        sent: Arc<Mutex<Vec<String>>>,
    }

    impl ChannelHandler for TestChannel {
        fn name(&self) -> &str {
            "web"
        }

        fn start(&self, _sink: ChannelSink) -> ChannelResult<ChannelRuntime> {
            Ok(ChannelRuntime::default())
        }

        fn send(&self, message: ChannelOutbound<'_>) -> ChannelResult<()> {
            if let Some(text) = message.text {
                self.sent.lock().unwrap().push(text.to_owned());
            }
            Ok(())
        }
    }
}
