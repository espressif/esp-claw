//! Building an agent of a kind through the production [`FsAgentFactory`].
//!
//! [`FsAgentFactory`] turns a kind into a real, running [`GenericAgent`] by tying
//! four pieces together:
//!
//! 1. the kind's compile-time-baked [`AgentManifest`](super::manifest::AgentManifest)
//!    (pure data: prompt + tool/skill *names*),
//! 2. a live LLM client minted per agent from a shared config + transport,
//! 3. central tool projection into each agent's `ToolSet`, and
//! 4. the factory-owned memory layout below one persistence root: transcripts,
//!    editable profile documents, and long-term memory.
//!
//! The orchestrator instance calls [`FsAgentFactory::create_agent`] for every
//! root and subagent, handing it the graph host and an [`AgentPlacement`] that
//! identifies either the session root or a spawned subagent. The `goal` is
//! seeded as the agent's first user message so it starts working immediately.

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::{ClawApiAsync, ClawApiConfig, InitError};
use claw_context::Block;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{
    LongTermInitError, LongTermMemory, ProfileStore, TranscriptInitError, TranscriptStore,
};
use claw_skill::{FsSkillRegistry, SkillError};
use claw_tool::{ToolRegistry, ToolSetError};

use crate::agent::base_agent::{AgentCommand, AgentCommandError, AgentId, BaseAgentBuildError};
use crate::agent::config::{AgentConfig, AgentConfigError};
use crate::agent::generic_agent::{GenericAgent, GenericAgentBuildError};
use crate::agent::graph::GraphHost;
use crate::agent::kind::AgentKind;
use crate::agent::manifest::AgentManifest;
use crate::agent::Agent;
use crate::memory::{
    agent_store, global_store, Extractor, LlmExtractor, LongTermMemoryContextAdapter,
    ProfileContextAdapter, RuleBasedTierClassifier, TierClassifier,
};
use crate::session::SessionId;

const TRANSCRIPT_DIR: &str = "sessions";
const PROFILE_DIR: &str = "profile";
const LONG_TERM_DIR: &str = "long_term";
const GLOBAL_LONG_TERM_DIR: &str = "global";
const AGENT_LONG_TERM_DIR: &str = "agents";
/// Subdirectory of the sessions root holding durable session-root transcripts,
/// keyed by [`SessionId`] so they survive restarts.
const ROOT_TRANSCRIPT_DIR: &str = "roots";
/// Subdirectory of the sessions root holding ephemeral subagent transcripts,
/// keyed by [`AgentId`] (subagent graphs never resume across a restart).
const SUB_TRANSCRIPT_DIR: &str = "agents";

/// Where a built agent sits in a session graph.
///
/// This is not just a transcript selector: it is the single source of truth for
/// root-only tool/profile permissions and for transcript placement. The
/// transcript store key is deliberately decoupled from the volatile [`AgentId`]:
/// a session **root**'s record is keyed by the stable [`SessionId`] so it is found
/// again when the session is rehydrated after a restart, while a **subagent**'s
/// record is keyed by its [`AgentId`] (its graph is rebuilt from scratch on the
/// next boot and never resumes, so a fresh key is correct).
///
/// The two live under separate subdirectories so the numeric id spaces never
/// collide.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AgentPlacement {
    /// A session's user-facing root; its transcript key is the session id.
    Root(SessionId),
    /// A spawned subagent; its transcript key is the agent id.
    Sub(AgentId),
}

impl AgentPlacement {
    /// The `(subdirectory, conversation_id)` this placement maps to under the
    /// sessions transcript root.
    fn location(self) -> (&'static str, u32) {
        match self {
            AgentPlacement::Root(session) => (ROOT_TRANSCRIPT_DIR, session.0),
            AgentPlacement::Sub(agent) => (SUB_TRANSCRIPT_DIR, agent.0),
        }
    }
}

/// The long-term-memory collaborators shared across every agent a factory
/// builds: the one global store, the derived per-agent-kind store root, and the
/// routing/extraction policies.
///
/// Built once by [`FsAgentFactory::new`]; each agent then gets its own private
/// store under `<long_term_dir>/agents/<kind>` plus a clone of the shared global
/// store under `<long_term_dir>/global`, fronted by one
/// [`LongTermMemoryContextAdapter`].
struct LongTermDeps<F: ClawFs + 'static> {
    /// The single store shared by every agent (user-level facts). Cloned (an
    /// `Arc` bump) into each agent's adapter so all agents read/write one store.
    global: LongTermMemory<F>,
    /// Root under which each baked agent kind owns a private store directory.
    agent_root_dir: String,
    /// Routes a new fact to the global or per-agent tier.
    classifier: Arc<dyn TierClassifier>,
    /// Distills durable facts from the transcript.
    extractor: Arc<dyn Extractor>,
}

impl<F: ClawFs + 'static> LongTermDeps<F> {
    /// Build the shared long-term collaborators from the explicit long-term
    /// memory root. The Rust memory runtime owns the internal layout below that
    /// root: `global` for shared facts and `agents/<kind>` for each baked agent
    /// kind's private memory.
    fn from_root(
        long_term_dir: &str,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Result<Self, LongTermInitError> {
        let global_dir = join_storage_path(long_term_dir, GLOBAL_LONG_TERM_DIR);
        let agent_root_dir = join_storage_path(long_term_dir, AGENT_LONG_TERM_DIR);
        Ok(Self {
            global: global_store::<F>(&global_dir)?,
            agent_root_dir,
            classifier,
            extractor,
        })
    }
}

struct FsAgentFactoryLayout {
    transcript_dir: String,
    profile_dir: String,
    long_term_dir: String,
}

impl FsAgentFactoryLayout {
    fn new(root: String) -> Self {
        Self {
            transcript_dir: join_storage_path(&root, TRANSCRIPT_DIR),
            profile_dir: join_storage_path(&root, PROFILE_DIR),
            long_term_dir: join_storage_path(&root, LONG_TERM_DIR),
        }
    }
}

/// What can go wrong while building an [`FsAgentFactory`].
#[derive(Debug, thiserror::Error)]
pub enum FsAgentFactoryError {
    /// No persistence directory was provided to the factory.
    #[error("persistence directory is required")]
    MissingPersistenceDir,
    /// The dedicated extraction LLM client (for long-term memory) failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(#[from] InitError),
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(#[from] LongTermInitError),
    /// The configured skill catalog could not be scanned.
    #[error("failed to load skill catalog: {0}")]
    SkillRegistry(#[from] SkillError),
}

/// What can go wrong while building one concrete agent from the factory.
#[derive(Debug, thiserror::Error)]
pub(crate) enum FsAgentCreateError {
    /// The baked manifest could not be resolved into an agent config.
    #[error("failed to resolve agent config: {0}")]
    Config(#[from] AgentConfigError),
    /// The agent's local tools could not be added to the tool set.
    #[error("failed to assemble agent tools: {0}")]
    Tools(#[from] ToolSetError),
    /// The transcript store for this placement could not be opened.
    #[error("failed to open transcript: {0}")]
    Transcript(#[from] TranscriptInitError),
    /// The generic agent failed to build.
    #[error("failed to build agent: {0}")]
    Agent(#[from] GenericAgentBuildError),
    /// The profile context adapter could not be attached.
    #[error("failed to attach profile context: {0}")]
    ProfileContext(#[source] BaseAgentBuildError),
    /// The per-agent long-term memory store could not be opened.
    #[error("failed to load long-term memory: {0}")]
    LongTerm(#[from] LongTermInitError),
    /// The long-term memory context adapter could not be attached.
    #[error("failed to attach long-term memory context: {0}")]
    LongTermContext(#[source] BaseAgentBuildError),
    /// The initial goal could not be enqueued.
    #[error("failed to seed initial goal: {0}")]
    Goal(#[from] AgentCommandError),
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

/// Builds real [`GenericAgent`]s from compile-time manifests.
///
/// Constructed by [`Orchestrator::new`](crate::Orchestrator::new); each
/// per-session instance uses it for root agents and spawned subagents.
pub struct FsAgentFactory<
    Filesystem: ClawFs + 'static,
    Http: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
> {
    /// Template for the per-agent LLM client. Each agent gets its own `ClawApi`
    /// minted from this config plus a freshly constructed `Http::default()`
    /// transport, so no transport instance is shared between agents.
    llm_config: ClawApiConfig,
    /// Central tool registry used to seed each agent tool set.
    tools: Arc<ToolRegistry>,
    /// Marks the HTTP transport type minted per agent. `fn() -> Http` so the
    /// marker is independent of owning an `Http` value (the factory only
    /// produces `Http`).
    _http: PhantomData<fn() -> Http>,
    /// Timer type minted per agent for async retry backoff.
    _timer: PhantomData<fn() -> Timer>,
    /// Directory for transcript files; each agent keys its own files by id.
    transcript_dir: String,
    /// Long-term-memory collaborators, shared across every agent this factory
    /// builds. Required: every agent gets a private store plus a clone of the
    /// shared global store, fronted by one [`LongTermMemoryContextAdapter`].
    long_term: LongTermDeps<Filesystem>,
    /// Global editable profile documents, fronted by one [`ProfileContextAdapter`]
    /// per agent so file edits are observed on the next context build.
    profile: ProfileStore<Filesystem>,
    /// Shared skill catalog scanned from the configured roots. Cloned (an `Arc`
    /// bump) into every agent's [`SkillSet`] so all agents share one catalog and a
    /// reload is observed everywhere.
    skills: Arc<FsSkillRegistry<Filesystem>>,
}

impl<
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<Filesystem, Http, Timer>
{
    /// Build a factory over an LLM `llm_config` and one persistence root.
    ///
    /// The factory owns the memory layout below `persistence_dir`: transcripts,
    /// editable profile documents, and long-term memory. `Filesystem` selects
    /// the static filesystem HAL backend used by those stores.
    ///
    /// # Errors
    ///
    /// Returns [`FsAgentFactoryError::MissingPersistenceDir`] when the
    /// persistence root is blank, or [`FsAgentFactoryError::ExtractionLlm`] if the
    /// internal extraction LLM client cannot be initialized.
    pub fn new(
        tools: Arc<ToolRegistry>,
        llm_config: ClawApiConfig,
        persistence_dir: String,
        skill_roots: Vec<String>,
    ) -> Result<Self, FsAgentFactoryError> {
        let span = tracing::info_span!("agent.factory");
        let _enter = span.enter();
        if persistence_dir.trim().is_empty() {
            tracing::error!(name: "missing_persistence_dir", reason = "empty");
            return Err(FsAgentFactoryError::MissingPersistenceDir);
        }
        let layout = FsAgentFactoryLayout::new(persistence_dir);

        let extraction_llm = match ClawApiAsync::<Http, Timer>::init_default(llm_config.clone()) {
            Ok(llm) => llm,
            Err(error) => {
                tracing::error!(name: "extraction_llm_init_failed", kind = "init");
                return Err(FsAgentFactoryError::ExtractionLlm(error));
            }
        };
        let long_term = match LongTermDeps::from_root(
            &layout.long_term_dir,
            RuleBasedTierClassifier::shared(),
            LlmExtractor::shared(extraction_llm),
        ) {
            Ok(deps) => deps,
            Err(error) => {
                tracing::error!(name: "long_term_memory_init_failed", kind = "init");
                return Err(error.into());
            }
        };

        let profile = ProfileStore::new(&layout.profile_dir);
        let skills = build_skill_registry::<Filesystem>(skill_roots)?;

        Ok(Self {
            llm_config,
            tools,
            _http: PhantomData,
            _timer: PhantomData,
            transcript_dir: layout.transcript_dir,
            long_term,
            profile,
            skills,
        })
    }
}

/// Build the shared skill catalog from the priority-ordered `skill_roots`.
///
/// A missing root is skipped so the agent still starts; a real scan failure
/// (e.g. a malformed `SKILL.md`) aborts construction.
fn build_skill_registry<F: ClawFs + 'static>(
    skill_roots: Vec<String>,
) -> Result<Arc<FsSkillRegistry<F>>, SkillError> {
    let span = tracing::info_span!("skill.catalog");
    let _enter = span.enter();
    let mut registry = FsSkillRegistry::<F>::new();
    for root in skill_roots {
        if !F::exists(root.as_str()) {
            tracing::warn!(name: "root_missing", "");
            continue;
        }
        match registry.set_root(root) {
            Ok(next) => registry = next,
            Err(error) => {
                tracing::warn!(name: "scan_failed", kind = "set_root");
                return Err(error);
            }
        }
    }
    Ok(Arc::new(registry))
}

impl<
        Filesystem: ClawFs + 'static,
        Http: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<Filesystem, Http, Timer>
{
    /// Build an agent of `kind` with id `id` already tasked with `goal`, handing
    /// it `host` as its back-channel to the agent graph. Used for both spawned
    /// subagents and a session's root agent.
    ///
    /// `placement` selects the durable transcript this agent attaches to: a
    /// root keys its record by the stable [`SessionId`] (so it resumes across
    /// restarts), a subagent by its [`AgentId`]. It also decides root-only tool
    /// wiring, so root/subagent identity has one source of truth.
    ///
    /// # Errors
    ///
    /// Returns a typed error when `kind` is unknown or the agent cannot be
    /// assembled; callers decide where to render it for logs or user-facing
    /// errors.
    pub(crate) fn create_agent(
        &self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        placement: AgentPlacement,
        host: Arc<dyn GraphHost>,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Box<dyn Agent>, FsAgentCreateError> {
        let span = tracing::info_span!("agent.create");
        let _enter = span.enter();
        // The config is pure data. Registry tools are projected here, then
        // manifest tools are added as local tools before the agent sees them.
        let mut config = self.resolve_config(kind).map_err(|error| {
            match &error {
                AgentConfigError::UnknownKind(_) => {
                    tracing::error!(name: "unknown_kind", kind = %kind.as_str());
                }
                AgentConfigError::UnknownTool(tool) => {
                    tracing::error!(
                        name: "unknown_tool",
                        kind = %kind.as_str(),
                        tool = %tool,
                    );
                }
            }
            FsAgentCreateError::Config(error)
        })?;
        let is_root = matches!(placement, AgentPlacement::Root(_));
        let mut tools = self.tools.tool_set();
        for tool in config.tools.drain(..) {
            if let Err(error) = tools.add_tool(tool) {
                tracing::error!(
                    name: "unknown_tool",
                    kind = %kind.as_str(),
                    tool = "registry",
                );
                return Err(FsAgentCreateError::Tools(error));
            }
        }

        // Roots and subagents live in separate subtrees keyed by session id vs
        // agent id, so a root transcript is found again on restart while subagent
        // transcripts stay ephemeral (and their id space never collides).
        let (subdir, conversation_id) = placement.location();
        let transcript_dir = join_storage_path(&self.transcript_dir, subdir);
        let store = match TranscriptStore::<Filesystem>::new(conversation_id, &transcript_dir) {
            Ok(store) => store,
            Err(error) => {
                tracing::error!(
                    name: "transcript_open_failed",
                    agent = %id,
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::Transcript(error));
            }
        };
        // The LLM client (and its transport) is built inside the agent from this
        // shared config plus the factory's transport type; nothing is minted here.
        let mut agent = match GenericAgent::<Http, Timer>::new(
            id,
            self.llm_config.clone(),
            store,
            config,
            tools,
            host,
            is_root,
            inherited_context,
        ) {
            Ok(agent) => agent,
            Err(error) => {
                tracing::error!(
                    name: "agent_build_failed",
                    agent = %id,
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::Agent(error));
            }
        };

        let profile_adapter = ProfileContextAdapter::new(self.profile.clone(), is_root);
        if let Err(error) = agent.register_context_adapter(Box::new(profile_adapter)) {
            tracing::error!(
                name: "context_adapter_attach_failed",
                agent = %id,
                adapter = "profile",
                kind = %kind.as_str(),
            );
            return Err(FsAgentCreateError::ProfileContext(error));
        }

        // Attach long-term memory (always): a per-agent-kind store derived from
        // the explicit long-term root plus a clone of the shared global store.
        let long_term = &self.long_term;
        let agent_dir = join_storage_path(&long_term.agent_root_dir, kind.as_str());
        let agent_memory = match agent_store::<Filesystem>(&agent_dir) {
            Ok(store) => store,
            Err(error) => {
                tracing::error!(
                    name: "context_adapter_attach_failed",
                    agent = %id,
                    adapter = "long_term",
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::LongTerm(error));
            }
        };
        let adapter = LongTermMemoryContextAdapter::new(
            agent_memory,
            long_term.global.clone(),
            Arc::clone(&long_term.classifier),
            Arc::clone(&long_term.extractor),
        );
        if let Err(error) = agent.register_context_adapter(Box::new(adapter)) {
            tracing::error!(
                name: "context_adapter_attach_failed",
                agent = %id,
                adapter = "long_term",
                kind = %kind.as_str(),
            );
            return Err(FsAgentCreateError::LongTermContext(error));
        }

        // The goal is the agent's first task: seed it as a user message so the
        // agent has something to work on as soon as it is ticked.
        if !goal.trim().is_empty() {
            if let Err(error) = agent.send_command(AgentCommand::AppendMessage(goal)) {
                tracing::error!(
                    name: "goal_seed_failed",
                    agent = %id,
                    kind = %kind.as_str(),
                );
                return Err(FsAgentCreateError::Goal(error));
            }
        }

        tracing::info!(name: "created", agent = %id, kind = %kind.as_str());
        Ok(Box::new(agent))
    }

    fn resolve_config(&self, kind: &AgentKind) -> Result<AgentConfig, AgentConfigError> {
        let manifest = AgentManifest::for_kind(kind.as_str())
            .ok_or_else(|| AgentConfigError::UnknownKind(kind.as_str().to_owned()))?;
        if let Some(name) = manifest.tools.first() {
            return Err(AgentConfigError::UnknownTool(name.as_str().to_owned()));
        }
        if !manifest.skills.is_empty() {
            tracing::info!(
                name: "manifest_ids_catalog_only",
                count = manifest.skills.len() as u64,
            );
        }
        Ok(AgentConfig::from_manifest(
            manifest,
            Vec::new(),
            self.skills.skill_set(),
        ))
    }
}
