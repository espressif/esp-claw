//! `claw_agent` — the concise external interface to the claw agent system.
//!
//! Everything under `rust/` (the orchestrator, the agent factory, channels,
//! memory, and the LLM client) is wired together here behind one small surface:
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
//! }).await?;
//! # Ok(())
//! # }
//! # }
//! ```

mod capability;
mod channel_router;

use std::sync::Arc;

use channel_router::ChannelRouter;
use claw_core::agent::{AgentResolver, FsAgentFactory, FsAgentFactoryError};
use claw_core::Orchestrator;
use claw_interface::{ClawHttp, ClawTimer};
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
pub use claw_core::{DeliverError, SessionError, SessionId, SessionRecord};
pub use claw_interface::ClawFs;
pub use claw_tool::{
    init_tool_executor, tool_metadata, AsyncToolHandler, Tool, ToolError, ToolFuture, ToolHandler,
    ToolInvocation, ToolInvokeError, ToolOutput, ToolRetryCount,
};
// The on-disk filesystem backend is a host-target convenience; device builds
// inject their own `ClawFs` through `AgentSystem::new::<F, H, Timer>(...)`.
#[cfg(feature = "host-backends")]
pub use claw_interface::DiskFs;

/// Explicit persistence root for an [`AgentSystem`].
///
/// Callers provide one directory. The agent system owns the layout below it:
/// transcripts live under `sessions`, editable profile documents under
/// `profile`, and long-term memory under `long_term`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentPersistenceConfig {
    dir: String,
}

impl AgentPersistenceConfig {
    /// Build persistence config from the required root directory.
    pub fn new(dir: &str) -> Self {
        Self {
            dir: dir.to_string(),
        }
    }
}

/// What can go wrong while building an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// No persistence directory was provided.
    #[error("persistence directory is required")]
    MissingPersistenceDir,
    /// The fixed tool-call executor could not start.
    #[error("failed to start the tool executor: {0}")]
    ToolExecutor(#[source] std::io::Error),
    /// The shared conversation-compaction LLM client failed to init.
    #[error("failed to initialize the compaction LLM client: {0}")]
    CompactorLlm(String),
    /// The dedicated extraction LLM client (for long-term memory) failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(String),
    /// A channel capability had an empty id.
    #[error("channel id is required")]
    MissingChannelId,
    /// Two registered channel capabilities used the same id.
    #[error("duplicate channel id: {0}")]
    DuplicateChannel(String),
}

impl From<FsAgentFactoryError> for AgentError {
    fn from(error: FsAgentFactoryError) -> Self {
        match error {
            FsAgentFactoryError::MissingPersistenceDir => Self::MissingPersistenceDir,
            FsAgentFactoryError::CompactorLlm(message) => Self::CompactorLlm(message),
            FsAgentFactoryError::ExtractionLlm(message) => Self::ExtractionLlm(message),
        }
    }
}

/// A ready-to-drive agent runtime.
///
/// Wraps a channel router plus a `claw_core::Orchestrator`. Registered channel
/// capabilities are opened through [`start`](AgentSystem::start); event-router
/// style direct submissions use [`push_message`](AgentSystem::push_message).
pub struct AgentSystem {
    registry: Arc<Registry>,
    router: Arc<ChannelRouter>,
}

impl AgentSystem {
    /// Build a fully injectable agent system. Use this for device builds, tests,
    /// or custom backends; for the common host case prefer
    /// [`AgentSystem::on_disk`].
    ///
    /// `F` is the concrete [`ClawFs`] backing all persistence (conversation and
    /// long-term memory) and `H` is the concrete async HTTP transport every LLM
    /// client speaks through. The system constructs both internally via
    /// [`Default`], so callers choose them by *type* — [`DiskFs`] +
    /// [`RealHttp`] on a host, in-memory/scripted doubles in tests — and
    /// never pass an instance; pair `F` with
    /// the required [`AgentPersistenceConfig`], which fixes where files land.
    /// A [`Registry`] is also required; pass an empty registry when no external
    /// capabilities are available. Each minted client (one per agent, plus the
    /// compaction and extraction clients) gets its own `H::default()` and
    /// `Timer::default()`.
    /// Custom runtimes that drive tools must also initialize the fixed tool
    /// executor once at boot with [`init_tool_executor`] and their platform
    /// `ClawThread` backend; [`on_disk`](AgentSystem::on_disk) does this for the
    /// host-backends path.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError::MissingPersistenceDir`] when the required
    /// persistence root is blank;
    /// [`AgentError::CompactorLlm`] / [`AgentError::ExtractionLlm`] if an
    /// internal LLM client fails to init.
    pub fn new<F, H, Timer>(
        llm_config: ClawApiConfig,
        persistence: AgentPersistenceConfig,
        registry: Arc<Registry>,
    ) -> Result<Self, AgentError>
    where
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    {
        let persistence_dir = persistence.dir;
        if persistence_dir.trim().is_empty() {
            return Err(AgentError::MissingPersistenceDir);
        }
        // The capability registry is the source of truth for tools and channel
        // transports.
        let resolver: Arc<dyn AgentResolver> =
            Arc::new(RegistryResolver::new(Arc::clone(&registry)));

        let factory = Arc::new(FsAgentFactory::<F, H, Timer>::new(
            resolver,
            llm_config,
            &persistence_dir,
        )?);

        let orchestrator = Orchestrator::new(factory);
        let router = ChannelRouter::new(orchestrator, registry.as_ref())?;

        Ok(Self { registry, router })
    }

    /// Build a host-target agent system backed by real disk memory and live HTTP
    /// transport.
    ///
    /// `persistence` provides the agent system's root persistence directory.
    /// The system derives its internal layout below that root. `registry`
    /// supplies every device capability/channel; pass an empty registry when no
    /// capabilities should be exposed.
    ///
    /// Host-target convenience (requires the `host-backends` feature): it
    /// constructs the [`DiskFs`] / [`RealHttp`] / [`StdThread`] backends
    /// directly. Device builds use
    /// [`AgentSystem::new::<F, H, Timer>(...)`](Self::new) with injected
    /// ESP-IDF backends instead.
    ///
    /// # Errors
    ///
    #[cfg(feature = "host-backends")]
    pub fn on_disk(
        llm: ClawApiConfig,
        persistence: AgentPersistenceConfig,
        registry: Arc<Registry>,
    ) -> Result<AgentSystem, AgentError> {
        init_tool_executor(StdThread).map_err(AgentError::ToolExecutor)?;
        AgentSystem::new::<DiskFs, RealHttp, TokioTimer>(llm, persistence, registry)
    }

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
