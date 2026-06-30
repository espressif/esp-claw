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
//! 4. per-agent on-disk conversation memory (one base dir; the agent keys its own
//!    files by id).
//!
//! The orchestrator instance calls
//! [`create_agent`](AgentFactory::create_agent) for every root and subagent,
//! handing it the graph host (always) and the root flag (only a session root gets
//! the `respond_to_approval` tool). The `goal` is seeded as the agent's first
//! user message so it starts working immediately.

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::{ClawApi, ClawApiConfig};
use claw_context::Block;
use claw_interface::http::ClawHttp;
use claw_interface::ClawFs;
use claw_memory::{LongTermMemory, TranscriptConfig};

use crate::agent::base_agent::{AgentCommand, AgentId};
use crate::agent::config::AgentConfig;
use crate::agent::generic_agent::{CompactionDeps, GenericAgent};
use crate::agent::graph::GraphHost;
use crate::agent::kind::AgentKind;
use crate::agent::resolver::AgentResolver;
use crate::agent::Agent;
use crate::memory::{agent_store, Extractor, LongTermMemoryAdapter, TierClassifier};

/// Creates a concrete agent for a goal.
///
/// Injected so the agent runtime stays free of LLM/memory wiring and
/// unit-testable with a fake factory. The factory wires the new agent's tools to
/// the provided [`GraphHost`] so it can spawn children and (for a root) resolve
/// approvals.
pub trait AgentFactory: Send + Sync {
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
/// builds: the one global store, the base directory for per-agent stores, and
/// the routing/extraction policies.
///
/// Built once by the system wiring layer and handed to
/// [`FsAgentFactory::new`]; each agent then gets its own private store under
/// `agent_dir` plus a clone of the shared `global` store, fronted by one
/// [`LongTermMemoryAdapter`].
pub struct LongTermDeps<F: ClawFs + Clone + 'static> {
    /// The single store shared by every agent (user-level facts). Cloned (an
    /// `Arc` bump) into each agent's adapter so all agents read/write one store.
    global: LongTermMemory<F>,
    /// Base directory under which each agent's private store lives, keyed by id.
    agent_dir: String,
    /// Routes a new fact to the global or per-agent tier.
    classifier: Arc<dyn TierClassifier>,
    /// Distills durable facts from the transcript off the tick path.
    extractor: Arc<dyn Extractor>,
}

impl<F: ClawFs + Clone + 'static> LongTermDeps<F> {
    /// Bundle the shared long-term collaborators. `global` is the one shared
    /// store; `agent_dir` is the base directory under which each agent's private
    /// store is created (keyed by agent id).
    pub fn new(
        global: LongTermMemory<F>,
        agent_dir: impl Into<String>,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Self {
        Self {
            global,
            agent_dir: agent_dir.into(),
            classifier,
            extractor,
        }
    }
}

/// Builds real [`GenericAgent`]s from compile-time manifests, an injected
/// resolver, a shared LLM config/transport, and shared memory collaborators.
///
/// Construct one and hand it to the orchestrator via
/// [`with_agent_factory`](crate::OrchestratorBuilder::with_agent_factory); the
/// registry then uses it for every agent in every session.
pub struct FsAgentFactory<F: ClawFs + Clone + 'static, H: ClawHttp + Default + 'static> {
    /// Maps a manifest's capability/skill *names* to handler code.
    resolver: Arc<dyn AgentResolver>,
    /// Template for the per-agent LLM client. Each agent gets its own `ClawApi`
    /// minted from this config plus a freshly constructed `H::default()`
    /// transport, so no transport instance is shared between agents.
    llm_config: ClawApiConfig,
    /// Marks the HTTP transport type minted per agent. `fn() -> H` so the marker
    /// is unconditionally `Send + Sync` (the factory only *produces* `H`).
    _http: PhantomData<fn() -> H>,
    /// Base directory for conversation files; each agent keys its own files by id.
    memory_dir: String,
    /// Storage backend cloned into each agent's [`TranscriptStore`] (and its
    /// long-term store). `F` is a concrete, statically dispatched [`ClawFs`]; it
    /// must be `Clone` because every agent gets its own handle (use
    /// `Arc<ConcreteFs>` for a shared backend — `Arc<T>` is itself `ClawFs`).
    memory_fs: F,
    /// Compaction collaborators (pool + compactor + policy), cloned into each
    /// agent's rolling-summary adapter. These belong to the agent layer, not the
    /// transcript store, which never compacts.
    compaction: CompactionDeps,
    /// Long-term-memory collaborators, shared across every agent this factory
    /// builds. Required: every agent gets a private store plus a clone of the
    /// shared global store, fronted by one [`LongTermMemoryAdapter`].
    long_term: LongTermDeps<F>,
}

impl<F: ClawFs + Clone + 'static, H: ClawHttp + Default + 'static> FsAgentFactory<F, H> {
    /// Build a factory over the injected `resolver`, an LLM `llm_config`, and the
    /// memory base dir + collaborators. The HTTP transport `H` is chosen by type
    /// (like `F`); each agent gets its own `H::default()` instance.
    ///
    /// `memory_fs` is the storage backend the firmware/host already knows how to
    /// build (real disk fs on device, in-memory doubles in tests); it is cloned
    /// per agent. `compaction` (pool + compactor + policy) drives each agent's
    /// rolling-summary adapter. `long_term` is the shared long-term-memory
    /// collaborators every agent is fronted with.
    pub fn new(
        resolver: Arc<dyn AgentResolver>,
        llm_config: ClawApiConfig,
        memory_dir: impl Into<String>,
        memory_fs: F,
        compaction: CompactionDeps,
        long_term: LongTermDeps<F>,
    ) -> Self {
        Self {
            resolver,
            llm_config,
            _http: PhantomData,
            memory_dir: memory_dir.into(),
            memory_fs,
            compaction,
            long_term,
        }
    }

    /// Mint a fresh LLM client for one agent: the shared config plus this
    /// factory's transport type, freshly constructed so each agent owns an
    /// independent `H`.
    fn mint_llm(&self) -> Result<ClawApi<H>, String> {
        ClawApi::init(self.llm_config.clone(), H::default())
            .map_err(|error| format!("initializing LLM: {error}"))
    }

    /// A fresh [`CompactionDeps`] sharing this factory's collaborators (`Arc`
    /// clones of the pool/compactor plus the `Copy` policy).
    fn clone_compaction(&self) -> CompactionDeps {
        CompactionDeps {
            pool: Arc::clone(&self.compaction.pool),
            compactor: Arc::clone(&self.compaction.compactor),
            policy: self.compaction.policy,
        }
    }
}

impl<F: ClawFs + Clone + 'static, H: ClawHttp + Default + Send + 'static> AgentFactory
    for FsAgentFactory<F, H>
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

        let transcript_config = TranscriptConfig::new(self.memory_dir.clone());
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

        // Attach long-term memory (always): a per-agent store under the base dir
        // (keyed by id) plus a clone of the shared global store.
        let long_term = &self.long_term;
        let agent_dir = format!("{}/{}", long_term.agent_dir.trim_end_matches('/'), id);
        let adapter = LongTermMemoryAdapter::new(
            agent_store(agent_dir, self.memory_fs.clone()),
            long_term.global.clone(),
            Arc::clone(&long_term.classifier),
            Arc::clone(&long_term.extractor),
            Arc::clone(&self.compaction.pool),
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
    use claw_interface::{MemFs, SharedScriptHttp, StdThread};
    use claw_memory::NoopCompactor;
    use claw_utils::{PoolConfig, SharedTaskPool};
    use serde_json::json;

    use super::*;
    use crate::agent::base_agent::TickOutcome;
    use crate::agent::graph::GraphEffect;
    use crate::memory::{global_store, CompactionPolicy, NoopExtractor, RuleBasedTierClassifier};
    use claw_skill::{SkillError, SkillId, SkillSet};
    use claw_tool::Tool;

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
    fn factory(bodies: Vec<String>) -> FsAgentFactory<MemFs, SharedScriptHttp> {
        SharedScriptHttp::install(bodies);
        let llm_config = ClawApiConfig {
            api_key: Some("sk-test".into()),
            backend_type: "openai_compatible".into(),
            model: Some("gpt-test".into()),
            base_url: Some("https://example.invalid".into()),
            supports_tools: true,
            ..Default::default()
        };
        let compaction = CompactionDeps {
            pool: Arc::new(
                SharedTaskPool::new(PoolConfig::default(), StdThread).expect("memory pool"),
            ),
            compactor: Arc::new(NoopCompactor),
            policy: CompactionPolicy::new(6000, 2000, 1500),
        };
        // Long-term memory is mandatory: build its (in-memory) collaborators. The
        // extractor is a no-op since these tests never trigger background
        // extraction; they only assert the agent builds and ticks.
        let long_term = LongTermDeps::new(
            global_store("/mem/long_term/global", MemFs::default()),
            "/mem/long_term/agents",
            RuleBasedTierClassifier::shared(),
            Arc::new(NoopExtractor),
        );
        FsAgentFactory::<MemFs, SharedScriptHttp>::new(
            Arc::new(EmptyResolver),
            llm_config,
            "/mem/agents",
            MemFs::default(),
            compaction,
            long_term,
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
            match agent.tick() {
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
}
