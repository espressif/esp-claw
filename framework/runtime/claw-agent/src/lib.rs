//! `claw_agent` — the concise external interface to the claw agent system.
//!
//! Everything under `rust/` (the orchestrator, the agent factory, channels,
//! scratch memory, and the LLM client) is wired together here behind one small
//! surface:
//!
//! - [`AgentSystem`] is a ready-to-drive agent runtime. Build it with the
//!   host-backend [`AgentSystem::on_disk`] (real disk memory + live HTTP,
//!   requires `host-backends`) or the fully injectable [`AgentSystem::new`]
//!   (for device, tests, or custom backends).
//! - Channels are registered as capabilities and opened with
//!   [`AgentSystem::start`]. A session must be explicitly bound with
//!   [`AgentSystem::bind_session`] before channel submissions go through
//!   [`AgentSystem::push_message`].
//!
//! Internally a message goes: channel or event router -> channel router ->
//! `claw_core::Orchestrator` (drives the per-session agent graph) -> registered
//! channel.
//!
//! # Examples
//!
//! ```no_run
//! # #[cfg(feature = "dev")]
//! # {
//! use std::sync::Arc;
//! use claw_agent::{AgentSystem, BackendKind, ClawApiConfig, InboundMessage, Registry};
//! use claw_agent::AgentPersistenceConfig;
//!
//! # #[tokio::main]
//! # async fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let llm = ClawApiConfig::new(
//!     BackendKind::OpenAiCompatible,
//!     "sk-...",
//!     "gpt-4o-mini",
//!     "https://api.openai.com/v1",
//! );
//!
//! let persistence = AgentPersistenceConfig::new("/tmp/claw");
//! let registry = Arc::new(Registry::new());
//! // Register a "local" channel capability in `registry` before starting.
//! let system = AgentSystem::on_disk(llm, persistence, registry)?;
//! let session = system.new_session();
//! system.bind_session(session, "local", "chat")?;
//! system.push_message(InboundMessage {
//!     message_id: "m1".into(),
//!     channel: "local".into(),
//!     chat_id: "chat".into(),
//!     sender_id: None,
//!     text: "hello".into(),
//!     ..Default::default()
//! }).await?;
//! # Ok(())
//! # }
//! # }
//! ```

mod capability;
mod channel_router;

use std::sync::Arc;

use channel_router::ChannelRouter;
use claw_capability::init_tool_executor;
use claw_core::{AgentResolver, Orchestrator, OrchestratorBuildError};
use claw_interface::{ClawHttp, ClawThread, ClawTimer, FsError};
#[cfg(feature = "host-backends")]
use claw_interface::{RealHttp, StdThread, TokioTimer};

// Re-exported so callers can configure the system without depending on the lower
// crates directly. These names are also used internally below.
pub use capability::RegistryResolver;
// The capability surface callers build their device from — re-exported so they
// depend on `claw_agent` alone, not the lower crates.
pub use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig};
pub use claw_capability::{
    Capability, CapabilityError, CapabilityGroup, CapabilityRole, CapabilityState, ChannelAdapter,
    ChannelFuture, ChannelRuntime, InboundMessage, Lifecycle, OutboundMessage, Registry,
};
// The tool-authoring vocabulary, re-exported *through* `claw_capability` (not
// `claw_tool`). A caller builds a Tool capability by implementing one of these
// handler traits, wrapping it with `Tool::new` / `Tool::new_async`, and handing
// that to `Capability::from_tool`; the rest of the tool framework (`ToolSet`, the
// executor) stays internal.
pub use claw_capability::{
    tool_metadata, AsyncToolHandler, Tool, ToolError, ToolFuture, ToolHandler, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolRetryCount,
};
pub use claw_core::{DeliverError, DeliveryKind, SessionError, SessionId};
pub use claw_interface::ClawFs;
// The on-disk filesystem backend is a host-target convenience; device builds
// inject their own `ClawFs` through `AgentSystem::<F, H, Timer>::new::<Thread>(...)`.
#[cfg(feature = "host-backends")]
pub use claw_interface::DiskFs;

#[cfg(feature = "host-backends")]
pub type HostAgentSystem = AgentSystem<DiskFs, RealHttp, TokioTimer>;

/// Explicit storage root for an [`AgentSystem`].
///
/// Callers provide one directory. The agent system clears it on construction and
/// owns the layout below it for the running process: transcripts live under
/// `sessions`, editable profile documents under `profile`, and long-term memory
/// under `long_term`.
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

/// External channel/chat bound to a live agent session.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionBinding {
    pub channel: String,
    pub chat_id: String,
}

impl SessionBinding {
    pub fn new(channel: impl Into<String>, chat_id: impl Into<String>) -> Self {
        Self {
            channel: channel.into(),
            chat_id: chat_id.into(),
        }
    }
}

/// One live conversation session as exposed by [`AgentSystem`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SessionRecord {
    pub id: SessionId,
    pub binding: Option<SessionBinding>,
}

/// What can go wrong while building an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// No storage directory was provided.
    #[error("agent storage directory is required")]
    MissingPersistenceDir,
    /// The fixed tool-call executor could not start.
    #[error("failed to start the tool executor: {0}")]
    ToolExecutor(#[source] std::io::Error),
    /// The dedicated extraction LLM client (for long-term memory) failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(String),
    /// A channel capability had an empty id.
    #[error("channel id is required")]
    MissingChannelId,
    /// Two registered channel capabilities used the same id.
    #[error("duplicate channel id: {0}")]
    DuplicateChannel(String),
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(String),
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
///
/// Wraps a channel router plus a `claw_core::Orchestrator`. Registered channel
/// capabilities are opened through [`start`](AgentSystem::start); event-router
/// style direct submissions use [`push_message`](AgentSystem::push_message).
pub struct AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    registry: Arc<Registry>,
    router: Arc<ChannelRouter<F, H, Timer>>,
}

impl<F, H, Timer> Clone for AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn clone(&self) -> Self {
        Self {
            registry: Arc::clone(&self.registry),
            router: Arc::clone(&self.router),
        }
    }
}

impl<F, H, Timer> AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Build a fully injectable agent system. Use this for device builds, tests,
    /// or custom backends; for the common host case prefer
    /// [`AgentSystem::on_disk`].
    ///
    /// `F` is the concrete [`ClawFs`] backing the agent system's scratch storage
    /// (conversation transcripts and long-term memory) and `H` is the concrete
    /// async HTTP transport every LLM client speaks through. The system constructs
    /// both internally via [`Default`], so callers choose them by *type* —
    /// [`DiskFs`] + [`RealHttp`] on a host, in-memory/scripted doubles in tests —
    /// and never pass an instance; pair `F` with the required
    /// [`AgentPersistenceConfig`], which fixes where files land. The storage root
    /// is cleared before any agent/session state is built, so the current runtime
    /// does not resume sessions across boot. A [`Registry`] is also required; pass
    /// an empty registry when no external capabilities are available. Each minted
    /// client (one per agent, plus the extraction client) gets its own
    /// `H::default()` and `Timer::default()`.
    ///
    /// The fixed tool executor is initialized here from `Thread::default()`, so
    /// callers never touch it directly — the tool framework stays an internal
    /// detail of the agent system.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError::MissingPersistenceDir`] when the required storage
    /// root is blank; [`AgentError::StorageClear`] if boot cleanup fails;
    /// [`AgentError::ToolExecutor`] if the tool executor thread cannot start;
    /// [`AgentError::ExtractionLlm`] if the extraction LLM client fails to init.
    pub fn new<Thread>(
        llm_config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
        registry: Arc<Registry>,
    ) -> Result<Self, AgentError>
    where
        Thread: ClawThread + Default + 'static,
    {
        let persistence_dir = persistence.dir;
        if persistence_dir.trim().is_empty() {
            return Err(AgentError::MissingPersistenceDir);
        }
        let storage = F::default();
        clear_storage_tree(&storage, &persistence_dir)?;

        // Own the fixed tool executor from within the system so callers never
        // initialize global tool state themselves. Idempotent: a repeated init
        // (e.g. multiple systems in one process) is a no-op.
        init_tool_executor(Thread::default()).map_err(AgentError::ToolExecutor)?;
        // The capability registry is the source of truth for tools and channel
        // transports.
        let resolver: Arc<dyn AgentResolver> =
            Arc::new(RegistryResolver::new(Arc::clone(&registry)));

        let orchestrator = Arc::new(Orchestrator::<F, H, Timer>::new(
            resolver,
            llm_config,
            &persistence_dir,
        )?);
        let router = ChannelRouter::new(orchestrator, registry.as_ref())?;

        Ok(Self { registry, router })
    }
}

#[cfg(feature = "host-backends")]
impl AgentSystem<DiskFs, RealHttp, TokioTimer> {
    /// Build a host-target agent system backed by real disk memory and live HTTP
    /// transport.
    ///
    /// `persistence` provides the agent system's root storage directory. The
    /// system clears it on construction and derives its internal layout below
    /// that root. `registry` supplies every device capability/channel; pass an
    /// empty registry when no capabilities should be exposed.
    ///
    /// Host-target convenience (requires the `host-backends` feature): it
    /// constructs the [`DiskFs`] / [`RealHttp`] / [`StdThread`] backends
    /// directly. Device builds use
    /// [`AgentSystem::<F, H, Timer>::new::<Thread>(...)`](Self::new) with
    /// injected ESP-IDF backends instead.
    ///
    /// # Errors
    ///
    pub fn on_disk(
        llm: ClawApiConfig,
        persistence: AgentPersistenceConfig,
        registry: Arc<Registry>,
    ) -> Result<Self, AgentError> {
        Self::new::<StdThread>(llm, persistence, registry)
    }
}

impl<F, H, Timer> AgentSystem<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    /// Open all registered channels and start capability lifecycles.
    ///
    /// # Errors
    ///
    /// Returns [`CapabilityError`] when a channel refuses to open or a registered
    /// lifecycle hook fails.
    pub fn start(&self) -> Result<(), CapabilityError> {
        self.router.open()?;
        if let Err(error) = self.registry.start_all() {
            let _ = self.router.close();
            return Err(error);
        }
        Ok(())
    }

    /// Open registered channels with an embedding-provided runtime, then start
    /// capability lifecycles.
    ///
    /// Embedding layers that already own the agent executor use this to hand
    /// channels a queue/sink runtime instead of the direct in-process router.
    ///
    /// # Errors
    ///
    /// Returns [`CapabilityError`] when a channel refuses to open or a registered
    /// lifecycle hook fails.
    pub fn start_with_runtime(
        &self,
        runtime: Arc<dyn ChannelRuntime>,
    ) -> Result<(), CapabilityError> {
        self.router.open_with_runtime(runtime)?;
        if let Err(error) = self.registry.start_all() {
            let _ = self.router.close();
            return Err(error);
        }
        Ok(())
    }

    /// Stop capability lifecycles and close registered channels.
    ///
    /// # Errors
    ///
    /// Returns the first close/stop failure.
    pub fn stop(&self) -> Result<(), CapabilityError> {
        let close_result = self.router.close();
        let lifecycle_result = self.registry.stop_all();
        close_result?;
        lifecycle_result
    }

    /// Submit one inbound channel message directly. The `(channel, chat_id)`
    /// pair must already be explicitly bound to a live session.
    ///
    /// # Errors
    ///
    /// Returns [`CapabilityError::NotFound`] when `message.channel` has no
    /// registered channel capability or the chat is not bound to a session.
    pub async fn push_message(&self, message: InboundMessage) -> Result<(), CapabilityError> {
        self.router.push_message(message).await
    }

    /// Submit `message` with [`DeliveryKind::Interrupt`]. In-flight effect (cutting
    /// a running drive short) requires the concurrent worker; a serial caller sees
    /// this as an ordinary append when nothing is running.
    ///
    /// # Errors
    ///
    /// See [`push_message`](Self::push_message).
    pub async fn interrupt(&self, message: InboundMessage) -> Result<(), CapabilityError> {
        self.router
            .push_with_kind(message, DeliveryKind::Interrupt)
            .await
    }

    /// Submit `message` with [`DeliveryKind::Cancel`], superseding any active task
    /// in the resolved session before this message starts a fresh one.
    ///
    /// # Errors
    ///
    /// See [`push_message`](Self::push_message).
    pub async fn cancel(&self, message: InboundMessage) -> Result<(), CapabilityError> {
        self.router
            .push_with_kind(message, DeliveryKind::Cancel)
            .await
    }

    /// Create a fresh, isolated conversation session and return its id.
    pub fn new_session(&self) -> SessionId {
        self.router.new_session()
    }

    /// Bind an existing session to an external channel chat.
    ///
    /// Inbound channel messages are accepted only after this explicit binding.
    ///
    /// # Errors
    ///
    /// Returns [`CapabilityError::NotFound`] when `session` is not live or
    /// `channel` is not a registered channel capability, and
    /// [`CapabilityError::AlreadyExists`] when the session or chat is already
    /// bound to a different counterpart.
    pub fn bind_session(
        &self,
        session: SessionId,
        channel: &str,
        chat_id: &str,
    ) -> Result<(), CapabilityError> {
        self.router.bind_session(session, channel, chat_id)
    }

    /// Return the live conversation sessions.
    pub fn list_sessions(&self) -> Vec<SessionRecord> {
        self.router.list_sessions()
    }

    /// Delete a conversation session and drop its live agent graph.
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::NotFound`] when `session` is not live.
    pub fn delete_session(&self, session: SessionId) -> Result<(), CapabilityError> {
        self.router.delete_session(session)
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

fn clear_storage_tree<F: ClawFs>(fs: &F, path: &str) -> Result<(), AgentError> {
    match fs.list_dir(path) {
        Ok(entries) => {
            for entry in entries {
                let child = join_storage_path(path, &entry);
                clear_storage_tree(fs, &child)?;
            }
            // `ClawFs` has no portable directory-delete operation. Removing the
            // exact path clears flat backends when this is a file key; directory
            // errors are ignored after their contents have been removed.
            let _ = fs.remove(path);
            Ok(())
        }
        Err(FsError::NotFound) => fs.remove(path).map_err(|source| AgentError::StorageClear {
            path: path.to_string(),
            source,
        }),
        Err(source) => Err(AgentError::StorageClear {
            path: path.to_string(),
            source,
        }),
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use claw_interface::MemFs;

    use super::*;

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
}
