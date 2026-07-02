//! Building an agent of a kind: the [`AgentFactory`] seam and its production
//! implementation [`FsAgentFactory`].
//!
//! [`AgentFactory`] is the injected boundary the orchestrator instance calls to
//! construct every root and subagent, so the runtime stays free of LLM/memory
//! wiring and unit-testable with a fake factory. [`FsAgentFactory`] is the
//! concrete counterpart to those test factories: it turns a kind into a real,
//! running [`GenericAgent`] by tying four pieces together:
//!
//! 1. the kind's compile-time-baked [`AgentManifest`](super::manifest::AgentManifest)
//!    (pure data: prompt + capability/skill *names*),
//! 2. an injected [`AgentResolver`] that maps those names to handler *code*,
//! 3. a live LLM client minted per agent from a shared config + transport, and
//! 4. the factory-owned memory layout below one persistence root: transcripts,
//!    editable profile documents, and long-term memory.
//!
//! The orchestrator instance calls
//! [`create_agent`](AgentFactory::create_agent) for every root and subagent,
//! handing it the graph host (always) and the root flag (only a session root gets
//! the `respond_to_approval` tool). The `goal` is seeded as the agent's first
//! user message so it starts working immediately.

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::{ClawApiAsync, ClawApiConfig};
use claw_context::Block;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{LongTermMemory, ProfileConfig, ProfileStore, TranscriptConfig};

use crate::agent::base_agent::{AgentCommand, AgentId};
use crate::agent::config::AgentConfig;
use crate::agent::generic_agent::{CompactionDeps, GenericAgent};
use crate::agent::graph::GraphHost;
use crate::agent::kind::AgentKind;
use crate::agent::resolver::AgentResolver;
use crate::agent::Agent;
use crate::memory::{
    agent_store, global_store, CompactionPolicy, Extractor, LlmCompactor, LlmExtractor,
    LongTermMemoryContextAdapter, ProfileContextAdapter, ProfileTools, RuleBasedTierClassifier,
    TierClassifier,
};

const TRANSCRIPT_DIR: &str = "sessions";
const PROFILE_DIR: &str = "profile";
const LONG_TERM_DIR: &str = "long_term";
const GLOBAL_LONG_TERM_DIR: &str = "global";
const AGENT_LONG_TERM_DIR: &str = "agents";
const COMPACTION_TRIGGER_TOKENS: usize = 6000;
const COMPACTION_KEEP_RECENT_TOKENS: usize = 2000;
const COMPACTION_SEGMENT_TOKEN_BUDGET: usize = 1500;

/// Creates a concrete agent for a goal.
///
/// Injected so the agent runtime stays free of LLM/memory wiring and
/// unit-testable with a fake factory. The factory wires the new agent's tools to
/// the provided [`GraphHost`] so it can spawn children and (for a root) resolve
/// approvals.
pub trait AgentFactory {
    /// Build an agent of `kind` with id `id` already tasked with `goal`, handing
    /// it `host` as its back-channel to the agent graph. Used for both spawned
    /// subagents and a session's root agent.
    ///
    /// `is_root` is `true` only for a session **root**: the root is the one agent
    /// that talks to the user, so it (and only it) gets the `respond_to_approval`
    /// tool to feed user verdicts back to waiting subagents. Subagents pass
    /// `false`.
    ///
    /// `inherited_context` carries the scope-layered prose blocks injected from
    /// above (Global -> Session), shared as an `Arc<[Block]>` so every agent in a
    /// session references one computed set for byte-identical prefixes. Empty for
    /// a standalone agent.
    ///
    /// # Errors
    ///
    /// Returns a human-readable error string when `kind` is unknown or the agent
    /// cannot be assembled; the caller logs it and drops the spawn.
    fn create_agent(
        &self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        host: Arc<dyn GraphHost>,
        is_root: bool,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Box<dyn Agent>, String>;
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
    ) -> Self {
        let global_dir = join_storage_path(long_term_dir, GLOBAL_LONG_TERM_DIR);
        let agent_root_dir = join_storage_path(long_term_dir, AGENT_LONG_TERM_DIR);
        Self {
            global: global_store(&global_dir, fs),
            agent_root_dir,
            classifier,
            extractor,
        }
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
    /// The shared conversation-compaction LLM client failed to init.
    #[error("failed to initialize the compaction LLM client: {0}")]
    CompactorLlm(String),
    /// The dedicated extraction LLM client (for long-term memory) failed to init.
    #[error("failed to initialize the extraction LLM client: {0}")]
    ExtractionLlm(String),
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

/// Builds real [`GenericAgent`]s from compile-time manifests, an injected
/// resolver, a shared LLM config/transport, and shared memory collaborators.
///
/// Construct one and hand it to [`Orchestrator::new`](crate::Orchestrator::new);
/// the registry then uses it for every agent in every session.
pub struct FsAgentFactory<
    F: ClawFs + Clone + Default + 'static,
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
> {
    /// Maps a manifest's capability/skill *names* to handler code.
    resolver: Arc<dyn AgentResolver>,
    /// Template for the per-agent LLM client. Each agent gets its own `ClawApi`
    /// minted from this config plus a freshly constructed `H::default()`
    /// transport, so no transport instance is shared between agents.
    llm_config: ClawApiConfig,
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
    /// Compaction collaborators cloned into each agent's rolling-summary adapter.
    /// These belong to the agent layer, not the transcript store, which never
    /// compacts.
    compaction: CompactionDeps,
    /// Long-term-memory collaborators, shared across every agent this factory
    /// builds. Required: every agent gets a private store plus a clone of the
    /// shared global store, fronted by one [`LongTermMemoryContextAdapter`].
    long_term: LongTermDeps<F>,
    /// Global editable profile documents, fronted by one [`ProfileContextAdapter`]
    /// per agent so file edits are observed on the next context build.
    profile: ProfileStore<F>,
}

impl<
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<F, H, Timer>
{
    /// Build a factory over the injected `resolver`, an LLM `llm_config`, and one
    /// persistence root. The HTTP transport `H` is chosen by type (like `F`);
    /// each agent gets its own `H::default()` instance.
    ///
    /// The factory owns the memory layout below `persistence_dir`: transcripts,
    /// editable profile documents, and long-term memory. It constructs the
    /// storage backend with `F::default()` and clones that handle per agent.
    ///
    /// # Errors
    ///
    /// Returns [`FsAgentFactoryError::MissingPersistenceDir`] when the
    /// persistence root is blank, or
    /// [`FsAgentFactoryError::CompactorLlm`] /
    /// [`FsAgentFactoryError::ExtractionLlm`] if one of the internal LLM clients
    /// cannot be initialized.
    pub fn new(
        resolver: Arc<dyn AgentResolver>,
        llm_config: ClawApiConfig,
        persistence_dir: &str,
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
        );

        let profile = ProfileStore::new(ProfileConfig::new(&layout.profile_dir), storage.clone());

        let compaction_llm = ClawApiAsync::init(llm_config.clone(), H::default(), Timer::default())
            .map_err(|error| FsAgentFactoryError::CompactorLlm(error.to_string()))?;
        let compaction = CompactionDeps {
            compactor: Arc::new(LlmCompactor::new(compaction_llm)),
            policy: CompactionPolicy::new(
                COMPACTION_TRIGGER_TOKENS,
                COMPACTION_KEEP_RECENT_TOKENS,
                COMPACTION_SEGMENT_TOKEN_BUDGET,
            ),
        };

        Ok(Self {
            resolver,
            llm_config,
            _http: PhantomData,
            _timer: PhantomData,
            transcript_dir: layout.transcript_dir,
            storage,
            compaction,
            long_term,
            profile,
        })
    }

    /// Mint a fresh LLM client for one agent: the shared config plus this
    /// factory's transport type, freshly constructed so each agent owns an
    /// independent `H`.
    fn mint_llm(&self) -> Result<ClawApiAsync<H, Timer>, String> {
        ClawApiAsync::init(self.llm_config.clone(), H::default(), Timer::default())
            .map_err(|error| format!("initializing LLM: {error}"))
    }

    /// A fresh [`CompactionDeps`] sharing this factory's collaborators (`Arc`
    /// clone of the compactor plus the `Copy` policy).
    fn clone_compaction(&self) -> CompactionDeps {
        CompactionDeps {
            compactor: Arc::clone(&self.compaction.compactor),
            policy: self.compaction.policy,
        }
    }
}

impl<
        F: ClawFs + Clone + Default + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > AgentFactory for FsAgentFactory<F, H, Timer>
{
    fn create_agent(
        &self,
        id: AgentId,
        kind: &AgentKind,
        goal: String,
        host: Arc<dyn GraphHost>,
        is_root: bool,
        inherited_context: Arc<[Block<'static>]>,
    ) -> Result<Box<dyn Agent>, String> {
        // The config is pure data; which graph tools attach is decided in
        // `GenericAgent::new` from the manifest's `spawn.enabled`, the host's
        // presence, and `is_root`.
        let config = AgentConfig::resolve(kind.as_str(), self.resolver.as_ref())
            .map_err(|error| format!("resolving config for kind '{kind}': {error}"))?;

        let llm = self
            .mint_llm()
            .map_err(|error| format!("{error} for {id}"))?;
        let supports_tools = llm.profile().supports_tools();

        let transcript_config = TranscriptConfig::new(&self.transcript_dir);
        let mut agent = GenericAgent::new(
            id,
            llm,
            transcript_config,
            self.storage.clone(),
            self.clone_compaction(),
            config,
            Some(host),
            is_root,
            inherited_context,
        )
        .map_err(|error| format!("building {kind} agent {id}: {error}"))?;

        // Attach editable global profile context to every agent. Only a root agent
        // with tool-capable LLM gets the mutation tools; subagents read profile
        // through context but do not write it directly.
        let profile_tools = if is_root && supports_tools {
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
            agent_store(&agent_dir, self.storage.clone()),
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
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::Arc;
    use std::task::{Wake, Waker};

    use claw_api::BackendKind;
    use claw_interface::{BlockingHttpAdapter, ImmediateTimer, MemFs, SharedScriptHttp, StdThread};
    use serde_json::json;

    use super::*;
    use crate::agent::base_agent::TickOutcome;
    use crate::agent::graph::GraphEffect;
    use claw_skill::{SkillError, SkillId, SkillSet};
    use claw_tool::{init_tool_executor, Tool};

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        init_tool_executor(StdThread).expect("tool executor");
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    /// A resolver with no capabilities or skills — enough for the built-in
    /// manifests, whose capability/skill lists are currently empty.
    struct EmptyResolver;
    impl AgentResolver for EmptyResolver {
        fn resolve_tool(&self, _name: &str) -> Option<Tool> {
            None
        }
        fn build_skills(&self, _ids: &[SkillId]) -> Result<Option<SkillSet>, SkillError> {
            Ok(None)
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
            Arc::new(EmptyResolver),
            llm_config,
            "/mem",
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
                Arc::new(NoopHost),
                true,
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
            Arc::new(NoopHost),
            false,
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
            Arc::new(NoopHost),
            false,
            Arc::from([]),
        );
        assert!(result.is_ok());
    }
}
