//! `claw_agent` — the concise external interface to the claw agent system.
//!
//! Everything under `rust/` (the orchestrator, the agent factory, channels,
//! memory, and the LLM client) is wired together here behind one small surface:
//!
//! - [`AgentSystem`] is a ready-to-drive agent runtime. Build it with the
//!   host-friendly [`AgentSystem::on_disk`] (real disk memory + live HTTP) or the
//!   fully injectable [`AgentSystem::builder`] (for tests / custom backends).
//! - [`Chat`] is a single conversation: [`Chat::send`] hands the model a message
//!   and returns its reply text(s). A [`Chat`] owns its own session, so several
//!   chats on one [`AgentSystem`] stay isolated.
//!
//! Internally a message goes: [`Chat::send`] -> [`AgentSystem`] ingress ->
//! `claw_core::Orchestrator` (drives the per-session agent graph synchronously)
//! -> egress -> reply text returned to the caller.
//!
//! # Examples
//!
//! ```no_run
//! use claw_agent::{AgentSystem, ClawApiConfig};
//!
//! let llm = ClawApiConfig {
//!     api_key: Some("sk-...".into()),
//!     backend_type: "openai_compatible".into(),
//!     model: Some("gpt-4o-mini".into()),
//!     base_url: Some("https://api.openai.com/v1".into()),
//!     supports_tools: true,
//!     ..Default::default()
//! };
//!
//! let system = AgentSystem::on_disk(llm, "/tmp/claw-mem")?;
//! let chat = system.chat();
//! for reply in chat.send("hello") {
//!     println!("{reply}");
//! }
//! # Ok::<(), claw_agent::AgentError>(())
//! ```

mod capability;

use std::marker::PhantomData;
use std::sync::{Arc, Mutex};

use claw_core::agent::{CompactionDeps, FsAgentFactory, LongTermDeps};
use claw_core::{
    global_store, ChannelEgress, ChannelEgressHub, CompactionPolicy, LlmCompactor, LlmExtractor,
    Orchestrator, RecordingTransport, RuleBasedTierClassifier,
};
use claw_interface::http::ClawHttp;
#[cfg(feature = "dev")]
use claw_interface::{RealHttp, StdThread};

// Re-exported so callers can configure the system without depending on the lower
// crates directly. These names are also used internally below.
pub use capability::{register_channels, RegistryChannelTransport, RegistryResolver};
// The capability surface callers build their device from — re-exported so they
// depend on `claw_agent` alone, not the lower crates.
pub use claw_api::{ClawApi, ClawApiConfig};
pub use claw_capability::{
    Capability, CapabilityError, CapabilityGroup, CapabilityRole, CapabilityState, ChannelAdapter,
    Lifecycle, OutboundMessage, Registry,
};
pub use claw_core::agent::{AgentResolver, MapAgentResolver};
pub use claw_core::{
    tool_invoke_err, ChannelIngressSink, ChannelTransport, InboundCommand, InboundMessage,
    SessionId, Tool, ToolError, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput,
};
pub use claw_interface::ClawFs;
// The on-disk filesystem backend is a dev convenience; device builds inject
// their own `ClawFs` through `AgentSystem::builder::<F, H>()`.
#[cfg(feature = "dev")]
pub use claw_interface::DiskFs;
// The one process-wide background worker pool callers configure and inject
// (owned by `claw-utils` so non-memory subsystems can share it). The
// per-conversation memory bundle and the compaction policy stay internal — this
// crate assembles them.
pub use claw_utils::{PoolConfig, SharedTaskPool};

/// The channel id outbound replies are routed through. Callers never see it; it
/// only needs to be stable so the egress transport and reply route agree.
const DEFAULT_CHANNEL: &str = "claw";

/// What can go wrong while building an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// No LLM config was provided to the builder.
    #[error("LLM config is required")]
    MissingLlmConfig,
    /// No memory base directory was provided to the builder.
    #[error("memory directory is required")]
    MissingMemoryDir,
    /// No shared task pool was provided to the builder.
    #[error("a shared task pool is required")]
    MissingTaskPool,
    /// The background memory task pool could not start (e.g. thread spawn failed).
    #[error("failed to start the shared task pool: {0}")]
    MemoryPool(#[from] std::io::Error),
    /// The shared conversation-compaction LLM client failed to init.
    #[error("failed to initialize the compaction LLM client: {0}")]
    CompactorLlm(String),
    /// The dedicated extraction LLM client (for long-term memory) failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(String),
}

/// A ready-to-drive agent runtime.
///
/// Wraps a `claw_core::Orchestrator` plus the egress transport replies come back
/// on. Open a [`Chat`] with [`chat`](AgentSystem::chat) (one session) or drive
/// raw sessions with [`new_session`](AgentSystem::new_session) +
/// [`send`](AgentSystem::send).
pub struct AgentSystem {
    orchestrator: Arc<Orchestrator>,
    /// Records outbound replies; drained after each synchronous `send`.
    transport: Arc<RecordingTransport>,
    /// The egress hub replies route through. Held so callers can register extra
    /// channel transports after construction (see [`register_transport`]).
    egress: Arc<ChannelEgressHub>,
    channel: String,
    /// Monotonic source for inbound `message_id`s.
    next_message_id: Mutex<u64>,
}

impl AgentSystem {
    /// Start a fully injectable builder. Use this for tests or custom backends;
    /// for the common host case prefer [`AgentSystem::on_disk`].
    ///
    /// `F` is the concrete [`ClawFs`] backing all persistence (conversation and
    /// long-term memory) and `H` is the concrete [`ClawHttp`] transport every LLM
    /// client speaks through. The system constructs both internally via
    /// [`Default`], so callers choose them by *type* — [`DiskFs`] + [`RealHttp`]
    /// on a host, in-memory/scripted doubles in tests — and never pass an
    /// instance; pair `F` with [`memory_dir`](AgentSystemBuilder::memory_dir),
    /// which fixes where files land. Each minted client (one per agent, plus the
    /// compaction and extraction clients) gets its own `H::default()`.
    pub fn builder<F, H>() -> AgentSystemBuilder<F, H>
    where
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + Send + 'static,
    {
        AgentSystemBuilder::default()
    }

    /// Build a dev agent system backed by real disk memory and a live HTTP
    /// transport, with no extra capabilities/skills resolver.
    ///
    /// `memory_dir` is the base directory under which each agent keys its own
    /// conversation files.
    ///
    /// Dev convenience (requires the default `dev` feature): it constructs the
    /// [`DiskFs`] / [`RealHttp`] / [`StdThread`] backends directly. Device builds
    /// disable `dev` and use [`AgentSystem::builder::<F, H>()`](Self::builder)
    /// with injected backends instead.
    ///
    /// # Errors
    ///
    /// Returns [`AgentError::MemoryPool`] if the background memory task pool
    /// cannot be created.
    #[cfg(feature = "dev")]
    pub fn on_disk(
        llm: ClawApiConfig,
        memory_dir: impl Into<String>,
    ) -> Result<AgentSystem, AgentError> {
        let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
        let memory_dir = memory_dir.into();
        // `DiskFs::default()` is verbatim-path mode; `memory_dir` is already an
        // absolute host path, so conversation files land beneath it and long-term
        // memory (always on) lands under `<memory_dir>/long_term`.
        AgentSystem::builder::<DiskFs, RealHttp>()
            .llm(llm)
            .task_pool(pool)
            .memory_dir(memory_dir)
            .build()
    }

    /// The inbound seam: where channels push user messages into the system.
    ///
    /// Returns the orchestrator as a [`ChannelIngressSink`]. A channel
    /// capability (or any transport) calls
    /// [`push_user_message`](ChannelIngressSink::push_user_message) with an
    /// [`InboundMessage`] whose `session_id` names an existing session (open one
    /// with [`new_session`](Self::new_session)) and whose `channel` matches a
    /// registered egress transport, so the reply is routed back out to it.
    pub fn ingress(&self) -> Arc<dyn ChannelIngressSink> {
        Arc::clone(&self.orchestrator) as Arc<dyn ChannelIngressSink>
    }

    /// Register an additional outbound [`ChannelTransport`] after construction.
    ///
    /// Capabilities supplied via
    /// [`AgentSystemBuilder::capabilities`] are registered automatically; use
    /// this for transports added later. Replies are routed to the transport
    /// whose [`id`](ChannelTransport::id) matches the inbound message's channel.
    pub fn register_transport(&self, transport: Arc<dyn ChannelTransport>) {
        self.egress.register(transport);
    }

    /// Create a fresh, isolated conversation session and return its id.
    pub fn new_session(&self) -> SessionId {
        self.orchestrator.session_create()
    }

    /// Open a new [`Chat`] (its own session) on this system.
    pub fn chat(&self) -> Chat<'_> {
        let session = self.new_session();
        Chat {
            system: self,
            session,
        }
    }

    /// Deliver `text` to `session` and return the reply text(s) the agent
    /// produced this turn.
    ///
    /// The orchestrator drives the session's agent graph synchronously, so by the
    /// time this returns every reply (and any surfaced approval prompt) for this
    /// turn has been routed and is collected here. Pending approvals appear as
    /// reply text tagged `[approval needed ...]`.
    pub fn send(&self, session: SessionId, text: impl Into<String>) -> Vec<String> {
        let id = {
            let mut next = self
                .next_message_id
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let id = *next;
            *next = next.saturating_add(1);
            id
        };
        self.orchestrator.push_user_message(InboundMessage {
            message_id: format!("m{id}"),
            channel: self.channel.clone(),
            chat_id: session.to_wire(),
            sender_id: None,
            session_id: session.to_wire(),
            text: text.into(),
        });
        self.transport
            .drain_sent()
            .into_iter()
            .map(|message| message.text)
            .collect()
    }
}

/// A single conversation bound to one session of an [`AgentSystem`].
///
/// Borrows the system so several chats can run against it without giving up
/// ownership; each chat keeps its own [`SessionId`].
pub struct Chat<'system> {
    system: &'system AgentSystem,
    session: SessionId,
}

impl Chat<'_> {
    /// This chat's underlying session id.
    pub fn session(&self) -> SessionId {
        self.session
    }

    /// Send a message and return the agent's reply text(s) for this turn.
    pub fn send(&self, text: impl Into<String>) -> Vec<String> {
        self.system.send(self.session, text)
    }
}

/// Builder for [`AgentSystem`]. Required: an LLM config, a memory directory, and
/// a [`task_pool`](Self::task_pool). Optional: the capability
/// [`Registry`](Self::capabilities) (or a raw [`AgentResolver`](Self::resolver))
/// and the egress channel id. Long-term memory is always on, rooted at
/// `<memory_dir>/long_term`; the conversation-compaction policy is internal —
/// callers do not supply one.
///
/// The persistence backend `F` and HTTP transport `H` are type parameters chosen
/// at [`AgentSystem::builder::<F, H>()`](AgentSystem::builder) and constructed
/// internally via [`Default`]; the builder stores no filesystem or transport
/// instance.
pub struct AgentSystemBuilder<F, H>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + Send + 'static,
{
    llm_config: Option<ClawApiConfig>,
    resolver: Option<Arc<dyn AgentResolver>>,
    /// Capability registry; when set it supplies the tool resolver and registers
    /// every available channel as an egress transport.
    capabilities: Option<Arc<Registry>>,
    memory_dir: Option<String>,
    /// The process-wide background worker pool (a system-level seam).
    task_pool: Option<Arc<SharedTaskPool>>,
    channel: String,
    /// Carries the persistence + transport types; the builder stores no `F`/`H`
    /// value (both are built via `Default` in [`build`](Self::build)). `fn() -> …`
    /// so the marker is unconditionally `Send + Sync`.
    marker: PhantomData<fn() -> (F, H)>,
}

impl<F, H> Default for AgentSystemBuilder<F, H>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + Send + 'static,
{
    fn default() -> Self {
        Self {
            llm_config: None,
            resolver: None,
            capabilities: None,
            memory_dir: None,
            task_pool: None,
            channel: DEFAULT_CHANNEL.to_string(),
            marker: PhantomData,
        }
    }
}

impl<F, H> AgentSystemBuilder<F, H>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + Send + 'static,
{
    /// Required: the LLM client config every agent is minted from.
    pub fn llm(mut self, config: ClawApiConfig) -> Self {
        self.llm_config = Some(config);
        self
    }

    /// Override the capability/skill resolver (default: empty
    /// [`MapAgentResolver`]). Ignored when [`capabilities`](Self::capabilities)
    /// is set — the registry then provides the resolver.
    pub fn resolver(mut self, resolver: Arc<dyn AgentResolver>) -> Self {
        self.resolver = Some(resolver);
        self
    }

    /// Drive the system from a capability [`Registry`] (the canonical surface).
    ///
    /// The registry's tools become the agent's resolver (via [`RegistryResolver`],
    /// superseding [`resolver`](Self::resolver)) and each of its available
    /// channels is registered as an outbound transport. Channels then exchange
    /// messages with the built system through
    /// [`AgentSystem::ingress`] (inbound) and their own
    /// [`ChannelAdapter`](claw_capability::ChannelAdapter) (outbound).
    pub fn capabilities(mut self, registry: Arc<Registry>) -> Self {
        self.capabilities = Some(registry);
        self
    }

    /// Required: base directory for conversation memory.
    pub fn memory_dir(mut self, dir: impl Into<String>) -> Self {
        self.memory_dir = Some(dir.into());
        self
    }

    /// Required: the process-wide [`SharedTaskPool`] that background jobs
    /// (conversation compaction, long-term extraction) run on. Build it once at
    /// boot with your platform's thread spawner and share it across the system.
    pub fn task_pool(mut self, pool: Arc<SharedTaskPool>) -> Self {
        self.task_pool = Some(pool);
        self
    }

    /// Override the egress channel id (default: `"claw"`). Rarely needed.
    pub fn channel(mut self, channel: impl Into<String>) -> Self {
        self.channel = channel.into();
        self
    }

    /// Assemble the [`AgentSystem`].
    ///
    /// # Errors
    ///
    /// Returns [`AgentError::MissingLlmConfig`], [`AgentError::MissingMemoryDir`],
    /// or [`AgentError::MissingTaskPool`] when a required input was not set;
    /// [`AgentError::CompactorLlm`] / [`AgentError::ExtractionLlm`] if an internal
    /// LLM client fails to init.
    pub fn build(self) -> Result<AgentSystem, AgentError> {
        let llm_config = self.llm_config.ok_or(AgentError::MissingLlmConfig)?;
        let memory_dir = self.memory_dir.ok_or(AgentError::MissingMemoryDir)?;
        // The persistence backend is built from its type — the caller chose `F`
        // via `builder::<F>()` and never passes an instance.
        let fs = F::default();
        let pool = self.task_pool.ok_or(AgentError::MissingTaskPool)?;
        // A capability registry, when given, is the source of truth for tools
        // (and its channels are registered as egress transports below); it
        // supersedes any explicit `resolver`.
        let capabilities = self.capabilities;
        let resolver: Arc<dyn AgentResolver> = match &capabilities {
            Some(registry) => Arc::new(RegistryResolver::new(Arc::clone(registry))),
            None => self
                .resolver
                .unwrap_or_else(|| Arc::new(MapAgentResolver::new())),
        };

        // Long-term memory is mandatory and always lives under the memory dir, so
        // it is built unconditionally before `fs` is moved into the memory deps
        // (the global store shares the same filesystem backend).
        let long_term_dir = format!("{}/long_term", memory_dir.trim_end_matches('/'));
        let long_term = {
            let global = global_store(format!("{long_term_dir}/global"), fs.clone());
            let extraction_llm = ClawApi::init(llm_config.clone(), H::default())
                .map_err(|error| AgentError::ExtractionLlm(error.to_string()))?;
            let extractor = LlmExtractor::shared(extraction_llm);
            let classifier = RuleBasedTierClassifier::shared();
            LongTermDeps::new(
                global,
                format!("{long_term_dir}/agents"),
                classifier,
                extractor,
            )
        };

        // The single conversation-compaction policy: summarize the aged window
        // through the configured LLM. One shared client backs every agent's
        // conversation memory; callers never choose or see it.
        let compaction_llm = ClawApi::init(llm_config.clone(), H::default())
            .map_err(|error| AgentError::CompactorLlm(error.to_string()))?;
        let compaction = CompactionDeps {
            pool,
            compactor: Arc::new(LlmCompactor::new(compaction_llm)),
            policy: CompactionPolicy::new(6000, 2000, 1500),
        };

        let factory = Arc::new(FsAgentFactory::<F, H>::new(
            resolver,
            llm_config,
            memory_dir,
            fs,
            compaction,
            long_term,
        ));

        let transport = RecordingTransport::new(self.channel.clone());
        let egress = Arc::new(ChannelEgressHub::new());
        egress.register(Arc::clone(&transport) as Arc<dyn ChannelTransport>);
        // Each registered capability channel becomes an outbound transport so the
        // orchestrator can route replies back to the channel they arrived on.
        if let Some(registry) = &capabilities {
            register_channels(registry, &egress);
        }

        let orchestrator = Orchestrator::builder()
            .config_egress(Arc::clone(&egress) as Arc<dyn ChannelEgress>)
            .with_agent_factory(factory)
            .build();

        Ok(AgentSystem {
            orchestrator,
            transport,
            egress,
            channel: self.channel,
            next_message_id: Mutex::new(1),
        })
    }
}
