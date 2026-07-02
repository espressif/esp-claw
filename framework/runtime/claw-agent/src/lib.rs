//! `claw_agent` — the concise external interface to the claw agent system.
//!
//! Everything under `rust/` (the orchestrator, the agent factory, channels,
//! memory, and the LLM client) is wired together here behind one small surface:
//!
//! - [`AgentSystem`] is a ready-to-drive agent runtime. Build it with the
//!   host-friendly [`AgentSystem::on_disk`] (real disk memory + live HTTP) or the
//!   fully injectable [`AgentSystem::builder`] (for tests / custom backends).
//! - Sessions are explicit: create one with [`AgentSystem::new_session`], then
//!   pass that [`SessionId`] to [`AgentSystem::send`].
//!
//! Internally a message goes: [`AgentSystem::send`] -> [`AgentSystem`] ingress ->
//! `claw_core::Orchestrator` (drives the per-session agent graph) -> egress ->
//! reply text returned to the caller.
//!
//! # Examples
//!
//! ```no_run
//! use claw_agent::{AgentSystem, BackendKind, ClawApiConfig};
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
//! let persistence = AgentPersistenceConfig::new(
//!     "/tmp/claw/sessions",
//!     "/tmp/claw/profile",
//!     "/tmp/claw/long_term/global",
//! )
//! .with_agent_long_term_dir("conversation", "/tmp/claw/long_term/agents/conversation")
//! .with_agent_long_term_dir("worker", "/tmp/claw/long_term/agents/worker");
//! let system = AgentSystem::on_disk(llm, persistence)?;
//! let session = system.new_session();
//! for reply in system.send(session, "hello").await? {
//!     println!("{reply}");
//! }
//! # Ok(())
//! # }
//! ```

mod capability;

use std::marker::PhantomData;
use std::sync::{Arc, Mutex};

use claw_core::agent::{AgentLongTermDirs, CompactionDeps, FsAgentFactory, LongTermDeps};
use claw_core::{
    global_store, ChannelEgress, ChannelEgressHub, CompactionPolicy, LlmCompactor, LlmExtractor,
    Orchestrator, RecordingTransport, RuleBasedTierClassifier,
};
use claw_interface::{ClawHttp, ClawTimer};
#[cfg(feature = "dev")]
use claw_interface::{RealHttp, StdThread, TokioTimer};
use claw_memory::{ProfileConfig, ProfileStore};

// Re-exported so callers can configure the system without depending on the lower
// crates directly. These names are also used internally below.
pub use capability::{register_channels, RegistryChannelTransport, RegistryResolver};
// The capability surface callers build their device from — re-exported so they
// depend on `claw_agent` alone, not the lower crates.
pub use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig};
pub use claw_capability::{
    Capability, CapabilityError, CapabilityGroup, CapabilityRole, CapabilityState, ChannelAdapter,
    Lifecycle, OutboundMessage, Registry,
};
pub use claw_core::agent::{AgentResolver, MapAgentResolver};
pub use claw_core::{
    ChannelIngressSink, ChannelTransport, DeliverError, InboundCommand, InboundMessage,
    IngressFuture, SessionError, SessionId, SessionRecord,
};
pub use claw_interface::ClawFs;
pub use claw_tool::{
    init_tool_executor, tool_metadata, AsyncToolHandler, Tool, ToolError, ToolFuture, ToolHandler,
    ToolInvocation, ToolInvokeError, ToolOutput, ToolRetryCount,
};
// The on-disk filesystem backend is a dev convenience; device builds inject
// their own `ClawFs` through `AgentSystem::builder::<F, H, Timer>()`.
#[cfg(feature = "dev")]
pub use claw_interface::DiskFs;

/// The channel id outbound replies are routed through. Callers never see it; it
/// only needs to be stable so the egress transport and reply route agree.
const DEFAULT_CHANNEL: &str = "claw";

/// Explicit persistence directories for an [`AgentSystem`].
///
/// This type carries final directories only. The framework does not derive
/// profile, transcript, or long-term-memory paths from a base directory.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentPersistenceConfig {
    transcript_dir: String,
    profile_dir: String,
    global_long_term_dir: String,
    agent_long_term_dirs: AgentLongTermDirs,
}

impl AgentPersistenceConfig {
    /// Build persistence config from final directories.
    pub fn new(transcript_dir: &str, profile_dir: &str, global_long_term_dir: &str) -> Self {
        Self {
            transcript_dir: transcript_dir.to_string(),
            profile_dir: profile_dir.to_string(),
            global_long_term_dir: global_long_term_dir.to_string(),
            agent_long_term_dirs: AgentLongTermDirs::new(),
        }
    }

    /// Add the final long-term-memory directory for one agent kind.
    pub fn with_agent_long_term_dir(mut self, kind: &str, dir: &str) -> Self {
        self.agent_long_term_dirs.insert(kind, dir);
        self
    }
}

/// What can go wrong while building an [`AgentSystem`].
#[derive(Debug, thiserror::Error)]
pub enum AgentError {
    /// No LLM config was provided to the builder.
    #[error("LLM config is required")]
    MissingLlmConfig,
    /// No transcript directory was provided to the builder.
    #[error("transcript directory is required")]
    MissingTranscriptDir,
    /// No profile directory was provided to the builder.
    #[error("profile directory is required")]
    MissingProfileDir,
    /// No global long-term memory directory was provided to the builder.
    #[error("global long-term memory directory is required")]
    MissingGlobalLongTermDir,
    /// No per-agent long-term memory directories were provided to the builder.
    #[error("at least one agent long-term memory directory is required")]
    MissingAgentLongTermDirs,
    /// The fixed tool-call executor could not start.
    #[error("failed to start the tool executor: {0}")]
    ToolExecutor(#[source] std::io::Error),
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
/// on. Create sessions explicitly with [`new_session`](AgentSystem::new_session)
/// and drive them with [`send`](AgentSystem::send).
pub struct AgentSystem {
    orchestrator: Arc<Orchestrator>,
    /// Records outbound replies; drained after each `send`.
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
    /// long-term memory) and `H` is the concrete async HTTP transport every LLM
    /// client speaks through. The system constructs both internally via
    /// [`Default`], so callers choose them by *type* — [`DiskFs`] +
    /// [`RealHttp`] on a host, in-memory/scripted doubles in tests — and
    /// never pass an instance; pair `F` with
    /// [`persistence`](AgentSystemBuilder::persistence), which fixes where files
    /// land. Each minted client (one per agent, plus the compaction and
    /// extraction clients) gets its own `H::default()` and `Timer::default()`.
    pub fn builder<F, H, Timer>() -> AgentSystemBuilder<F, H, Timer>
    where
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    {
        AgentSystemBuilder::default()
    }

    /// Build a dev agent system backed by real disk memory and a live HTTP
    /// transport, with no extra capabilities/skills resolver.
    ///
    /// `persistence` provides final directories for transcript, profile, global
    /// long-term memory, and each agent kind's long-term memory.
    ///
    /// Dev convenience (requires the default `dev` feature): it constructs the
    /// [`DiskFs`] / [`RealHttp`] / [`StdThread`] backends directly. Device builds
    /// disable `dev` and use [`AgentSystem::builder::<F, H, Timer>()`](Self::builder)
    /// with injected backends instead.
    ///
    /// # Errors
    ///
    #[cfg(feature = "dev")]
    pub fn on_disk(
        llm: ClawApiConfig,
        persistence: AgentPersistenceConfig,
    ) -> Result<AgentSystem, AgentError> {
        init_tool_executor(StdThread).map_err(AgentError::ToolExecutor)?;
        AgentSystem::builder::<DiskFs, RealHttp, TokioTimer>()
            .llm(llm)
            .persistence(persistence)
            .build()
    }

    /// Where channels push user messages into the system.
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

    /// Return the live conversation sessions.
    pub fn list_sessions(&self) -> Vec<SessionRecord> {
        self.orchestrator.session_list()
    }

    /// Delete a conversation session and drop its live agent graph.
    ///
    /// # Errors
    ///
    /// Returns [`SessionError::NotFound`] when `session` is not live.
    pub fn delete_session(&self, session: SessionId) -> Result<(), SessionError> {
        self.orchestrator.session_delete(session)
    }

    /// Deliver `text` to `session` and return the reply text(s) the agent
    /// produced this turn.
    ///
    /// By the time this future resolves every reply (and any surfaced approval
    /// prompt) for this turn has been routed and is collected here. Pending
    /// approvals appear as reply text tagged `[approval needed ...]`.
    ///
    /// # Errors
    ///
    /// Returns [`DeliverError`] when `claw_core` rejects the inbound message,
    /// including when `session` is not live.
    pub async fn send(
        &self,
        session: SessionId,
        text: impl Into<String>,
    ) -> Result<Vec<String>, DeliverError> {
        let id = {
            let mut next = self
                .next_message_id
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner());
            let id = *next;
            *next = next.saturating_add(1);
            id
        };
        self.orchestrator
            .push_user_message(InboundMessage {
                message_id: format!("m{id}"),
                channel: self.channel.clone(),
                chat_id: session.to_wire(),
                sender_id: None,
                session_id: session.to_wire(),
                text: text.into(),
            })
            .await?;
        Ok(self
            .transport
            .drain_sent()
            .into_iter()
            .map(|message| message.text)
            .collect())
    }
}

/// Builder for [`AgentSystem`]. Required: an LLM config and explicit persistence
/// directories. Optional: the capability
/// [`Registry`](Self::capabilities) (or a raw [`AgentResolver`](Self::resolver))
/// and the egress channel id. Long-term memory is always on; the
/// conversation-compaction policy is internal — callers do not supply one.
/// Custom runtimes that drive tools must also initialize the fixed tool executor
/// once at boot with [`init_tool_executor`] and their platform `ClawThread`
/// backend; [`on_disk`](AgentSystem::on_disk) does this for the dev host path.
///
/// The persistence backend `F`, async HTTP transport `H`, and `Timer` are type
/// parameters chosen at
/// [`AgentSystem::builder::<F, H, Timer>()`](AgentSystem::builder) and
/// constructed internally via [`Default`]; the builder stores no filesystem,
/// transport, or timer instance.
pub struct AgentSystemBuilder<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    llm_config: Option<ClawApiConfig>,
    resolver: Option<Arc<dyn AgentResolver>>,
    /// Capability registry; when set it supplies the tool resolver and registers
    /// every available channel as an egress transport.
    capabilities: Option<Arc<Registry>>,
    transcript_dir: Option<String>,
    profile_dir: Option<String>,
    global_long_term_dir: Option<String>,
    agent_long_term_dirs: AgentLongTermDirs,
    channel: String,
    /// Carries the persistence + async runtime types; the builder stores no
    /// `F`/`H`/`Timer` value (all are built via `Default` in
    /// [`build`](Self::build)). `fn() -> …` keeps the marker independent of
    /// owning runtime values.
    marker: PhantomData<fn() -> (F, H, Timer)>,
}

impl<F, H, Timer> Default for AgentSystemBuilder<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    fn default() -> Self {
        Self {
            llm_config: None,
            resolver: None,
            capabilities: None,
            transcript_dir: None,
            profile_dir: None,
            global_long_term_dir: None,
            agent_long_term_dirs: AgentLongTermDirs::new(),
            channel: DEFAULT_CHANNEL.to_string(),
            marker: PhantomData,
        }
    }
}

impl<F, H, Timer> AgentSystemBuilder<F, H, Timer>
where
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
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
    /// [`ChannelAdapter`] (outbound).
    pub fn capabilities(mut self, registry: Arc<Registry>) -> Self {
        self.capabilities = Some(registry);
        self
    }

    /// Required: set all persistence directories at once.
    pub fn persistence(mut self, config: AgentPersistenceConfig) -> Self {
        self.transcript_dir = Some(config.transcript_dir);
        self.profile_dir = Some(config.profile_dir);
        self.global_long_term_dir = Some(config.global_long_term_dir);
        self.agent_long_term_dirs = config.agent_long_term_dirs;
        self
    }

    /// Required: final directory for transcript files.
    pub fn transcript_dir(mut self, dir: &str) -> Self {
        self.transcript_dir = Some(dir.to_string());
        self
    }

    /// Required: final directory for editable profile documents.
    pub fn profile_dir(mut self, dir: &str) -> Self {
        self.profile_dir = Some(dir.to_string());
        self
    }

    /// Required: final directory for global long-term memory.
    pub fn global_long_term_dir(mut self, dir: &str) -> Self {
        self.global_long_term_dir = Some(dir.to_string());
        self
    }

    /// Required for every agent kind the runtime may build: final directory for
    /// that kind's private long-term memory.
    pub fn agent_long_term_dir(mut self, kind: &str, dir: &str) -> Self {
        self.agent_long_term_dirs.insert(kind, dir);
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
    /// Returns [`AgentError::MissingLlmConfig`],
    /// [`AgentError::MissingTranscriptDir`], [`AgentError::MissingProfileDir`],
    /// [`AgentError::MissingGlobalLongTermDir`],
    /// [`AgentError::MissingAgentLongTermDirs`] when a required input was not
    /// set; [`AgentError::CompactorLlm`] / [`AgentError::ExtractionLlm`] if an
    /// internal LLM client fails to init.
    pub fn build(self) -> Result<AgentSystem, AgentError> {
        let llm_config = self.llm_config.ok_or(AgentError::MissingLlmConfig)?;
        let transcript_dir = self
            .transcript_dir
            .ok_or(AgentError::MissingTranscriptDir)?;
        let profile_dir = self.profile_dir.ok_or(AgentError::MissingProfileDir)?;
        let global_long_term_dir = self
            .global_long_term_dir
            .ok_or(AgentError::MissingGlobalLongTermDir)?;
        if self.agent_long_term_dirs.is_empty() {
            return Err(AgentError::MissingAgentLongTermDirs);
        }
        // The persistence backend is built from its type — the caller chose `F`
        // via `builder::<F>()` and never passes an instance.
        let fs = F::default();
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

        // Long-term memory is mandatory and built from explicit final
        // directories before `fs` is moved into the memory deps.
        let long_term = {
            let global = global_store(&global_long_term_dir, fs.clone());
            let extraction_llm =
                ClawApiAsync::init(llm_config.clone(), H::default(), Timer::default())
                    .map_err(|error| AgentError::ExtractionLlm(error.to_string()))?;
            let extractor = LlmExtractor::shared(extraction_llm);
            let classifier = RuleBasedTierClassifier::shared();
            LongTermDeps::new(global, self.agent_long_term_dirs, classifier, extractor)
        };
        let profile = ProfileStore::new(ProfileConfig::new(&profile_dir), fs.clone());

        // The single conversation-compaction policy: summarize the aged window
        // through the configured LLM. One shared client backs every agent's
        // conversation memory; callers never choose or see it.
        let compaction_llm = ClawApiAsync::init(llm_config.clone(), H::default(), Timer::default())
            .map_err(|error| AgentError::CompactorLlm(error.to_string()))?;
        let compaction = CompactionDeps {
            compactor: Arc::new(LlmCompactor::new(compaction_llm)),
            policy: CompactionPolicy::new(6000, 2000, 1500),
        };

        let factory = Arc::new(FsAgentFactory::<F, H, Timer>::new(
            resolver,
            llm_config,
            &transcript_dir,
            fs,
            compaction,
            long_term,
            profile,
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
