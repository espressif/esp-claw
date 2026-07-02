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
//! 4. per-agent on-disk transcript storage (one explicit transcript dir; the
//!    agent keys its own files by id).
//!
//! The orchestrator instance calls
//! [`create_agent`](AgentFactory::create_agent) for every root and subagent,
//! handing it the graph host (always) and the root flag (only a session root gets
//! the `respond_to_approval` tool). The `goal` is seeded as the agent's first
//! user message so it starts working immediately.

use std::collections::BTreeMap;
use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::{ClawApiAsync, ClawApiConfig};
use claw_context::Block;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::{LongTermMemory, ProfileStore, TranscriptConfig};

use crate::agent::base_agent::{AgentCommand, AgentId};
use crate::agent::config::AgentConfig;
use crate::agent::generic_agent::{CompactionDeps, GenericAgent};
use crate::agent::graph::GraphHost;
use crate::agent::kind::AgentKind;
use crate::agent::resolver::AgentResolver;
use crate::agent::Agent;
use crate::memory::{
    agent_store, Extractor, LongTermMemoryContextAdapter, ProfileContextAdapter, ProfileTools,
    TierClassifier,
};

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

/// Explicit final long-term memory directories for agent kinds.
///
/// The factory never derives a path from a root. Assembly code owns the storage
/// layout and inserts the final directory for every kind it allows the runtime
/// to build.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AgentLongTermDirs {
    by_kind: BTreeMap<String, String>,
}

impl AgentLongTermDirs {
    /// Start an empty directory table.
    pub fn new() -> Self {
        Self {
            by_kind: BTreeMap::new(),
        }
    }

    /// Insert or replace the final long-term memory directory for `kind`.
    pub fn insert(&mut self, kind: &str, dir: &str) {
        self.by_kind.insert(kind.to_string(), dir.to_string());
    }

    /// Builder-style insertion for call sites that construct the table inline.
    pub fn with_dir(mut self, kind: &str, dir: &str) -> Self {
        self.insert(kind, dir);
        self
    }

    fn get(&self, kind: &AgentKind) -> Option<&str> {
        self.by_kind.get(kind.as_str()).map(String::as_str)
    }

    /// Whether the table has no configured kinds.
    pub fn is_empty(&self) -> bool {
        self.by_kind.is_empty()
    }
}

/// The long-term-memory collaborators shared across every agent a factory
/// builds: the one global store, explicit final per-kind store directories, and
/// the routing/extraction policies.
///
/// Built once by the system wiring layer and handed to
/// [`FsAgentFactory::new`]; each agent then gets its own private store from the
/// explicit per-kind directory table plus a clone of the shared `global` store,
/// fronted by one
/// [`LongTermMemoryContextAdapter`].
pub struct LongTermDeps<F: ClawFs + Clone + 'static> {
    /// The single store shared by every agent (user-level facts). Cloned (an
    /// `Arc` bump) into each agent's adapter so all agents read/write one store.
    global: LongTermMemory<F>,
    /// Final directory for each agent kind's private store.
    agent_dirs: AgentLongTermDirs,
    /// Routes a new fact to the global or per-agent tier.
    classifier: Arc<dyn TierClassifier>,
    /// Distills durable facts from the transcript.
    extractor: Arc<dyn Extractor>,
}

impl<F: ClawFs + Clone + 'static> LongTermDeps<F> {
    /// Bundle the shared long-term collaborators. `global` is the one shared
    /// store; `agent_dirs` maps each agent kind to its final private store
    /// directory.
    pub fn new(
        global: LongTermMemory<F>,
        agent_dirs: AgentLongTermDirs,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Self {
        Self {
            global,
            agent_dirs,
            classifier,
            extractor,
        }
    }

    fn agent_dir_for(&self, kind: &AgentKind) -> Option<&str> {
        self.agent_dirs.get(kind)
    }
}

/// Builds real [`GenericAgent`]s from compile-time manifests, an injected
/// resolver, a shared LLM config/transport, and shared memory collaborators.
///
/// Construct one and hand it to the orchestrator via
/// [`with_agent_factory`](crate::OrchestratorBuilder::with_agent_factory); the
/// registry then uses it for every agent in every session.
pub struct FsAgentFactory<
    F: ClawFs + Clone + 'static,
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
    memory_fs: F,
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
        F: ClawFs + Clone + 'static,
        H: ClawHttp + Default + 'static,
        Timer: ClawTimer + Default + 'static,
    > FsAgentFactory<F, H, Timer>
{
    /// Build a factory over the injected `resolver`, an LLM `llm_config`, and the
    /// transcript dir + collaborators. The HTTP transport `H` is chosen by type
    /// (like `F`); each agent gets its own `H::default()` instance.
    ///
    /// `memory_fs` is the storage backend the firmware/host already knows how to
    /// build (real disk fs on device, in-memory doubles in tests); it is cloned
    /// per agent. `compaction` drives each agent's rolling-summary adapter.
    /// `long_term` is the shared long-term-memory
    /// collaborators every agent is fronted with.
    pub fn new(
        resolver: Arc<dyn AgentResolver>,
        llm_config: ClawApiConfig,
        transcript_dir: &str,
        memory_fs: F,
        compaction: CompactionDeps,
        long_term: LongTermDeps<F>,
        profile: ProfileStore<F>,
    ) -> Self {
        Self {
            resolver,
            llm_config,
            _http: PhantomData,
            _timer: PhantomData,
            transcript_dir: transcript_dir.to_string(),
            memory_fs,
            compaction,
            long_term,
            profile,
        }
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
        F: ClawFs + Clone + 'static,
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
            self.memory_fs.clone(),
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

        // Attach long-term memory (always): a per-agent-kind store from the
        // explicit directory table plus a clone of the shared global store.
        let long_term = &self.long_term;
        let agent_dir = long_term
            .agent_dir_for(kind)
            .ok_or_else(|| format!("missing long-term memory dir for agent kind '{kind}'"))?;
        let adapter = LongTermMemoryContextAdapter::new(
            agent_store(agent_dir, self.memory_fs.clone()),
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
    use claw_memory::NoopCompactor;
    use serde_json::json;

    use super::*;
    use crate::agent::base_agent::TickOutcome;
    use crate::agent::graph::GraphEffect;
    use crate::memory::{global_store, CompactionPolicy, NoopExtractor, RuleBasedTierClassifier};
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
        SharedScriptHttp::install(bodies);
        let llm_config = ClawApiConfig::new(
            BackendKind::OpenAiCompatible,
            "sk-test",
            "gpt-test",
            "https://example.invalid",
        );
        let compaction = CompactionDeps {
            compactor: Arc::new(NoopCompactor),
            policy: CompactionPolicy::new(6000, 2000, 1500),
        };
        // Long-term memory is mandatory: build its (in-memory) collaborators. The
        // extractor is a no-op since these tests never trigger background
        // extraction; they only assert the agent builds and ticks.
        let long_term = LongTermDeps::new(
            global_store("/mem/long_term/global", MemFs::default()),
            AgentLongTermDirs::new().with_dir("conversation", "/mem/long_term/agents/conversation"),
            RuleBasedTierClassifier::shared(),
            Arc::new(NoopExtractor),
        );
        let profile = ProfileStore::new(
            claw_memory::ProfileConfig::new("/mem/profile"),
            MemFs::default(),
        );
        FsAgentFactory::<MemFs, BlockingHttpAdapter<SharedScriptHttp>, ImmediateTimer>::new(
            Arc::new(EmptyResolver),
            llm_config,
            "/mem/agents",
            MemFs::default(),
            compaction,
            long_term,
            profile,
        )
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
    fn missing_agent_long_term_dir_is_an_error() {
        let factory = factory(vec![]);
        let error = match factory.create_agent(
            AgentId(1),
            &AgentKind::new("worker"),
            "x".into(),
            Arc::new(NoopHost),
            false,
            Arc::from([]),
        ) {
            Ok(_) => panic!("worker dir is intentionally not configured"),
            Err(error) => error,
        };
        assert!(
            error.contains("missing long-term memory dir for agent kind 'worker'"),
            "{error}"
        );
    }
}
