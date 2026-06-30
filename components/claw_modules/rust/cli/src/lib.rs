//! Shared scaffolding for the agent CLIs.
//!
//! Both binaries in this crate (`base-agent` and `generic-agent-chat`) drive
//! a real agent against a live LLM with on-disk conversation memory. The platform
//! dependencies are identical, so the real-disk [`ClawFs`], live [`ClawHttp`], the
//! no-op [`Compactor`], and the env/LLM/memory wiring live here once.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`.

use std::path::Path;
use std::sync::Arc;

use claw_api::{ClawApi, ClawApiConfig};
use claw_core::agent::LongTermDeps;
use claw_core::{global_store, LlmExtractor, RuleBasedTierClassifier};
use claw_interface::{DiskFs, RealHttp, StdThread};
use claw_memory::{ConversationConfig, ConversationDeps, ConversationMemory, NoopCompactor};
use claw_utils::{PoolConfig, SharedTaskPool};

// The real network transport is `claw_interface::RealHttp` (the `realhttp`
// feature); background summarisation is disabled via claw-memory's
// `NoopCompactor` (the `compactor-stub` feature).

/// The concrete `ClawFs` the CLI runs over: the real disk backend. `DiskFs` is
/// itself a cheap clone handle (just a base path), so each agent gets a clone
/// and there is no need to wrap it in an `Arc`.
pub type CliFs = DiskFs;

// ---------------------------------------------------------------------------
// Config / wiring
// ---------------------------------------------------------------------------

/// Load `claw-core/.env.local` into the process environment if present.
///
/// # Panics
///
/// If the file exists but cannot be parsed — a misconfigured env file is a setup
/// error the operator should see, not silently ignore.
pub fn load_env() {
    let env_path = Path::new(env!("CARGO_MANIFEST_DIR")).join("../claw-core/.env.local");
    if env_path.is_file() {
        dotenvy::from_path(&env_path)
            .unwrap_or_else(|e| panic!("failed to load {}: {e}", env_path.display()));
    }
}

/// Build the LLM client config from the `CLAW_LLM_*` environment variables.
///
/// `supports_tools` enables tool-calling for agents that need it (the
/// conversation/orchestrator agents); pass `false` for a plain chat agent.
///
/// Returned separately from the transport so callers that mint clients
/// themselves (e.g. [`claw_core::agent::FsAgentFactory`], which inits one client
/// per agent) can reuse this config.
///
/// # Panics
///
/// If any required `CLAW_LLM_*` variable is missing — there is no safe default
/// for these, so fail loudly.
pub fn make_llm_config(supports_tools: bool) -> ClawApiConfig {
    let api_key = std::env::var("CLAW_LLM_API_KEY")
        .ok()
        .filter(|v| !v.is_empty())
        .expect("CLAW_LLM_API_KEY must be set");
    let base_url = std::env::var("CLAW_LLM_BASE_URL").expect("CLAW_LLM_BASE_URL must be set");
    let model = std::env::var("CLAW_LLM_MODEL").expect("CLAW_LLM_MODEL must be set");

    ClawApiConfig {
        api_key: Some(api_key),
        backend_type: "openai_compatible".into(),
        model: Some(model),
        base_url: Some(base_url),
        supports_tools,
        timeout_ms: 60_000,
        ..Default::default()
    }
}

/// The live network transport ([`RealHttp`]). Each LLM client owns its own.
pub fn make_http() -> RealHttp {
    RealHttp::new()
}

/// Build a live LLM client from the `CLAW_LLM_*` environment variables.
///
/// `supports_tools` enables tool-calling for agents that need it.
///
/// # Panics
///
/// If any required `CLAW_LLM_*` variable is missing, or the client cannot init.
pub fn make_llm(supports_tools: bool) -> ClawApi<RealHttp> {
    ClawApi::init(make_llm_config(supports_tools), make_http()).expect("failed to init LLM client")
}

/// Build the shared memory collaborators: the real disk [`ClawFs`], a fresh
/// background task pool, and the no-op compactor.
///
/// Public so factories that build their own memory (e.g.
/// [`claw_core::agent::FsAgentFactory`]) can take these collaborators directly.
///
/// # Panics
///
/// If the background memory task pool cannot be created.
pub fn make_memory_deps() -> ConversationDeps<CliFs> {
    let pool =
        Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread).expect("memory pool"));
    ConversationDeps {
        fs: DiskFs::absolute(),
        pool,
        compactor: Arc::new(NoopCompactor),
    }
}

/// Build an on-disk conversation memory at `memory_dir` plus a cloned read-only
/// view of the same memory (handy for a `/messages` command). For agents that
/// build their own memory (e.g. [`claw_core::agent::GenericAgent`]), use
/// [`make_memory_ingredients`] instead.
///
/// # Panics
///
/// If the background memory task pool cannot be created.
pub fn make_memory(
    agent_id: usize,
    memory_dir: &str,
) -> (ConversationMemory<CliFs>, ConversationMemory<CliFs>) {
    let memory = ConversationMemory::new(
        agent_id,
        ConversationConfig::new(memory_dir),
        make_memory_deps(),
    );
    let view = memory.clone();
    (memory, view)
}

/// Build the ingredients a [`claw_core::agent::GenericAgent`] needs to construct
/// its own on-disk memory at `memory_dir`: the base config plus the shared
/// collaborators. The agent keys the conversation by its own id, so no id is
/// needed here.
///
/// # Panics
///
/// If the background memory task pool cannot be created.
pub fn make_memory_ingredients(memory_dir: &str) -> (ConversationConfig, ConversationDeps<CliFs>) {
    (ConversationConfig::new(memory_dir), make_memory_deps())
}

/// Build the long-term-memory collaborators rooted at `base_dir` — mandatory for
/// every agent a [`claw_core::agent::FsAgentFactory`] builds: one shared global
/// store under `<base_dir>/global`, per-agent stores under `<base_dir>/agents`,
/// rule-based tier routing, and LLM-backed background extraction over a fresh
/// client.
///
/// # Panics
///
/// If the extraction LLM client cannot init (missing `CLAW_LLM_*`).
pub fn make_long_term_deps(base_dir: &str) -> LongTermDeps<CliFs> {
    let base_dir = base_dir.trim_end_matches('/');
    let global = global_store(format!("{base_dir}/global"), DiskFs::absolute());
    let extractor = LlmExtractor::shared(make_llm(true));
    LongTermDeps::new(
        global,
        format!("{base_dir}/agents"),
        RuleBasedTierClassifier::shared(),
        extractor,
    )
}
