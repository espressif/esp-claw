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

use claw_api::{ClawApiAsync, ClawApiConfig};
use claw_context::Block;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{
    LongTermInitError, LongTermMemory, ProfileConfig, ProfileStore, TranscriptConfig,
    TranscriptStore,
};
use claw_skill::{FsSkillRegistry, SkillSet};
use claw_tool::{Tool, ToolRegistry};

use crate::agent::base_agent::{AgentCommand, AgentId};
use crate::agent::config::{AgentConfig, AgentConfigError};
use crate::agent::generic_agent::GenericAgent;
use crate::agent::graph::GraphHost;
use crate::agent::kind::AgentKind;
use crate::agent::manifest::AgentManifest;
use crate::agent::Agent;
use crate::memory::{
    agent_store, global_store, Extractor, LlmExtractor, LongTermMemoryContextAdapter,
    ProfileContextAdapter, ProfileTools, RuleBasedTierClassifier, TierClassifier,
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
    /// Whether this placement belongs to the session root.
    fn is_root(self) -> bool {
        matches!(self, AgentPlacement::Root(_))
    }

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
struct LongTermDeps<F: ClawFs + Clone + 'static> {
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

impl<F: ClawFs + Clone + 'static> LongTermDeps<F> {
    /// Build the shared long-term collaborators from the explicit long-term
    /// memory root. The Rust memory runtime owns the internal layout below that
    /// root: `global` for shared facts and `agents/<kind>` for each baked agent
    /// kind's private memory.
    fn from_root(
        long_term_dir: &str,
        fs: F,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Result<Self, LongTermInitError> {
        let global_dir = join_storage_path(long_term_dir, GLOBAL_LONG_TERM_DIR);
        let agent_root_dir = join_storage_path(long_term_dir, AGENT_LONG_TERM_DIR);
        Ok(Self {
            global: global_store(&global_dir, fs)?,
            agent_root_dir,
            classifier,
            extractor,
        })
    }

    fn agent_dir_for(&self, kind: &AgentKind) -> String {
        join_storage_path(&self.agent_root_dir, kind.as_str())
    }
}

struct FsAgentFactoryLayout {
    transcript_dir: String,
    profile_dir: String,
    long_term_dir: String,
}

impl FsAgentFactoryLayout {
    fn new(root: &str) -> Self {
        Self {
            transcript_dir: join_storage_path(root, TRANSCRIPT_DIR),
            profile_dir: join_storage_path(root, PROFILE_DIR),
            long_term_dir: join_storage_path(root, LONG_TERM_DIR),
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
    ExtractionLlm(String),
    /// A long-term memory journal exists but could not be read at startup.
    #[error("failed to load long-term memory: {0}")]
    LongTermInit(#[from] LongTermInitError),
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
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
> {
    /// Template for the per-agent LLM client. Each agent gets its own `ClawApi`
    /// minted from this config plus a freshly constructed `H::default()`
    /// transport, so no transport instance is shared between agents.
    llm_config: ClawApiConfig,
    /// Central tool registry used to seed each agent tool set.
    tools: Arc<ToolRegistry>,
    /// Marks the HTTP transport type minted per agent. `fn() -> H` so the marker
    /// is independent of owning an `H` value (the factory only *produces* `H`).
    _http: PhantomData<fn() -> H>,
    /// Timer type minted per agent for async retry backoff.
    _timer: PhantomData<fn() -> Timer>,
    /// Directory for transcript files; each agent keys its own files by id.
    transcript_dir: String,
    /// Storage backend cloned into each agent's [`TranscriptStore`] (and its
    /// long-term store). `F` is a concrete, statically dispatched [`ClawFs`]; it
    /// must be `Clone` because every agent gets its own handle (use
    /// `Arc<ConcreteFs>` for a shared backend — `Arc<T>` is itself `ClawFs`).
    storage: F,
    /// Long-term-memory collaborators, shared across every agent this factory
    /// builds. Required: every agent gets a private store plus a clone of the
    /// shared global store, fronted by one [`LongTermMemoryContextAdapter`].
    long_term: LongTermDeps<F>,
    /// Global editable profile documents, fronted by one [`ProfileContextAdapter`]
    /// per agent so file edits are observed on the next context build.
    profile: ProfileStore<F>,
    /// Shared skill catalog scanned from the configured roots. Cloned (an `Arc`
    /// bump) into every agent's [`SkillSet`] so all agents share one catalog and a
    /// reload is observed everywhere.
    skills: Arc<FsSkillRegistry<F>>,
}

impl<
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<F, H, Timer>
{
    /// Build a factory over an LLM `llm_config` and one persistence root.
    ///
    /// The factory owns the memory layout below `persistence_dir`: transcripts,
    /// editable profile documents, and long-term memory. It constructs the
    /// storage backend with `F::default()` and clones that handle per agent.
    ///
    /// # Errors
    ///
    /// Returns [`FsAgentFactoryError::MissingPersistenceDir`] when the
    /// persistence root is blank, or [`FsAgentFactoryError::ExtractionLlm`] if the
    /// internal extraction LLM client cannot be initialized.
    pub fn new(
        tools: Arc<ToolRegistry>,
        llm_config: ClawApiConfig,
        persistence_dir: &str,
        skill_roots: &[String],
    ) -> Result<Self, FsAgentFactoryError> {
        if persistence_dir.trim().is_empty() {
            return Err(FsAgentFactoryError::MissingPersistenceDir);
        }
        let layout = FsAgentFactoryLayout::new(persistence_dir);
        let storage = F::default();

        let extraction_llm = ClawApiAsync::init(llm_config.clone(), H::default(), Timer::default())
            .map_err(|error| FsAgentFactoryError::ExtractionLlm(error.to_string()))?;
        let long_term = LongTermDeps::from_root(
            &layout.long_term_dir,
            storage.clone(),
            RuleBasedTierClassifier::shared(),
            LlmExtractor::shared(extraction_llm),
        )?;

        let profile = ProfileStore::new(ProfileConfig::new(&layout.profile_dir), storage.clone());
        let skills = build_skill_registry(&storage, skill_roots);

        Ok(Self {
            llm_config,
            tools,
            _http: PhantomData,
            _timer: PhantomData,
            transcript_dir: layout.transcript_dir,
            storage,
            long_term,
            profile,
            skills,
        })
    }
}

/// Build the shared skill catalog from the priority-ordered `skill_roots`.
///
/// A missing root is logged and skipped so the agent still starts; a real scan
/// failure (e.g. a malformed `SKILL.md`) disables skills entirely by falling back
/// to an empty registry rather than aborting agent construction.
fn build_skill_registry<F: ClawFs + Clone + 'static>(
    storage: &F,
    skill_roots: &[String],
) -> Arc<FsSkillRegistry<F>> {
    let mut registry = FsSkillRegistry::new(storage.clone());
    for root in skill_roots {
        if !storage.exists(root) {
            tracing::error!(root = %root, "skill root directory is missing; skipping");
            continue;
        }
        match registry.set_root(root.as_str()) {
            Ok(next) => registry = next,
            Err(error) => {
                tracing::error!(
                    root = %root,
                    error = %error,
                    "failed to scan skill root; disabling filesystem skills"
                );
                return Arc::new(FsSkillRegistry::new(storage.clone()));
            }
        }
    }
    Arc::new(registry)
}

impl<
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<F, H, Timer>
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
    /// Returns a human-readable error string when `kind` is unknown or the agent
    /// cannot be assembled; the caller logs it and drops the spawn.
    pub(crate) fn create_agent(
        &self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        placement: AgentPlacement,
        host: Arc<dyn GraphHost>,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Box<dyn Agent>, String> {
        // The config is pure data. Registry tools are projected here, then
        // manifest tools are added as local tools before the agent sees them.
        let mut config = self
            .resolve_config(kind)
            .map_err(|error| format!("resolving config for kind '{kind}': {error}"))?;
        let is_root = placement.is_root();
        let mut tools = self.tools.tool_set();
        for tool in config.tools.drain(..) {
            tools
                .add_tool(tool)
                .map_err(|error| format!("assembling tools for {kind} agent {id}: {error}"))?;
        }

        // Roots and subagents live in separate subtrees keyed by session id vs
        // agent id, so a root transcript is found again on restart while subagent
        // transcripts stay ephemeral (and their id space never collides).
        let (subdir, conversation_id) = placement.location();
        let transcript_dir = join_storage_path(&self.transcript_dir, subdir);
        let transcript_config = TranscriptConfig::new(&transcript_dir);
        let store = TranscriptStore::new(conversation_id, transcript_config, self.storage.clone())
            .map_err(|error| format!("opening transcript for {kind} agent {id}: {error}"))?;
        // The LLM client (and its transport) is built inside the agent from this
        // shared config plus the factory's transport type; nothing is minted here.
        let mut agent = GenericAgent::<H, Timer>::new(
            id,
            self.llm_config.clone(),
            store,
            config,
            tools,
            host,
            is_root,
            inherited_context,
        )
        .map_err(|error| format!("building {kind} agent {id}: {error}"))?;

        // Attach editable global profile context to every agent. Only a root agent
        // gets the mutation tools; subagents read profile through context but do
        // not write it directly.
        let profile_tools = if is_root {
            ProfileTools::Writable
        } else {
            ProfileTools::Disabled
        };
        let profile_adapter = ProfileContextAdapter::new(self.profile.clone(), profile_tools);
        agent
            .register_context_adapter(Box::new(profile_adapter))
            .map_err(|error| format!("attaching profile context to {id}: {error}"))?;

        // Attach long-term memory (always): a per-agent-kind store derived from
        // the explicit long-term root plus a clone of the shared global store.
        let long_term = &self.long_term;
        let agent_dir = long_term.agent_dir_for(kind);
        let adapter = LongTermMemoryContextAdapter::new(
            agent_store(&agent_dir, self.storage.clone())
                .map_err(|error| format!("loading long-term memory for {id}: {error}"))?,
            long_term.global.clone(),
            Arc::clone(&long_term.classifier),
            Arc::clone(&long_term.extractor),
        );
        agent
            .register_context_adapter(Box::new(adapter))
            .map_err(|error| format!("attaching long-term memory to {id}: {error}"))?;

        // The goal is the agent's first task: seed it as a user message so the
        // agent has something to work on as soon as it is ticked.
        if !goal.trim().is_empty() {
            agent
                .send_command(AgentCommand::AppendMessage(goal))
                .map_err(|error| format!("seeding goal for {id}: {error}"))?;
        }

        Ok(Box::new(agent))
    }

    fn resolve_config(&self, kind: &AgentKind) -> Result<AgentConfig, AgentConfigError> {
        let manifest = AgentManifest::for_kind(kind.as_str())
            .ok_or_else(|| AgentConfigError::UnknownKind(kind.as_str().to_owned()))?;
        let tools = Self::resolve_manifest_tools(manifest)?;
        let skills = self.resolve_manifest_skills(manifest);
        Ok(AgentConfig::from_manifest(manifest, tools, skills))
    }

    fn resolve_manifest_tools(
        manifest: &'static AgentManifest,
    ) -> Result<Vec<Tool>, AgentConfigError> {
        if let Some(name) = manifest.tools.first() {
            return Err(AgentConfigError::UnknownTool(name.as_str().to_owned()));
        }
        Ok(Vec::new())
    }

    /// Project the shared filesystem skill catalog into a per-agent [`SkillSet`].
    ///
    /// The manifest's skill ids are catalog-only today: every agent sees the full
    /// scanned catalog rather than a manifest-filtered subset.
    fn resolve_manifest_skills(&self, manifest: &'static AgentManifest) -> SkillSet {
        if !manifest.skills.is_empty() {
            tracing::debug!(
                kind = %manifest.kind,
                count = manifest.skills.len(),
                "manifest skill ids are catalog-only"
            );
        }
        self.skills.skill_set()
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use claw_api::BackendKind;
    use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp};
    use serde_json::json;

    use super::*;
    use crate::agent::base_agent::TickOutcome;
    use crate::agent::graph::GraphEffect;
    use claw_tool::ToolRegistry;

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    /// A graph host that is never expected to fire in these single-agent tests.
    struct NoopHost;
    impl GraphHost for NoopHost {
        fn next_id(&self) -> AgentId {
            AgentId(0)
        }
        fn emit(&self, _requester: AgentId, _effect: GraphEffect) {}
        fn snapshot(&self) -> Vec<crate::agent::graph::AgentSnapshot> {
            Vec::new()
        }
    }

    fn body_plain_text(text: &str) -> String {
        json!({ "choices": [{ "message": { "role": "assistant", "content": text } }] }).to_string()
    }

    /// The factory mints each agent's transport internally via `H::default()`, so
    /// the script can't be injected as an instance — it is installed into the
    /// thread-local `SharedScriptHttp` script that every minted client shares.
    fn factory(
        bodies: Vec<String>,
    ) -> FsAgentFactory<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer> {
        let mut script = Vec::with_capacity(bodies.len().saturating_mul(2));
        for body in bodies {
            script.push(body_plain_text("[]"));
            script.push(body);
        }
        SharedScriptHttp::install(script);
        let llm_config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        );
        FsAgentFactory::<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>::new(
            Arc::new(ToolRegistry::new()),
            llm_config,
            "/mem",
            &[],
        )
        .expect("factory builds")
    }

    #[test]
    fn builds_a_runnable_agent_seeded_with_its_goal() {
        let factory = factory(vec![body_plain_text("hello there")]);
        let mut agent = factory
            .create_agent(
                AgentId(1),
                &AgentKind::new("conversation"),
                "say hi".into(),
                AgentPlacement::Root(SessionId(1)),
                Arc::new(NoopHost),
                Arc::from([]),
            )
            .expect("agent builds");

        // The goal was seeded, so ticking drives straight to the scripted reply.
        let outcome = loop {
            match block_on(agent.tick()) {
                TickOutcome::Working => continue,
                other => break other,
            }
        };
        match outcome {
            TickOutcome::Yielded { text } => assert_eq!(text, "hello there"),
            other => panic!("unexpected outcome: {other:?}"),
        }
    }

    #[test]
    fn unknown_kind_is_an_error() {
        let factory = factory(vec![]);
        let result = factory.create_agent(
            AgentId(1),
            &AgentKind::new("nope"),
            "x".into(),
            AgentPlacement::Sub(AgentId(1)),
            Arc::new(NoopHost),
            Arc::from([]),
        );
        assert!(result.is_err());
    }

    #[test]
    fn worker_kind_gets_derived_long_term_dir() {
        let factory = factory(vec![]);
        let result = factory.create_agent(
            AgentId(1),
            &AgentKind::new("worker"),
            "x".into(),
            AgentPlacement::Sub(AgentId(1)),
            Arc::new(NoopHost),
            Arc::from([]),
        );
        assert!(result.is_ok());
    }
}
