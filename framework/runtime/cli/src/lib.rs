//! Shared scaffolding for the agent CLIs.
//!
//! Both binaries in this crate (`base-agent` and `generic-agent-chat`) drive
//! a real agent against a live LLM with on-disk conversation memory. The platform
//! dependencies are identical, so the real-disk [`ClawFs`], live async HTTP, the
//! no-op [`Compactor`], and the env/LLM/memory wiring live here once.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`.

use std::path::Path;
use std::sync::Arc;

use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig};
use claw_core::agent::CompactionDeps;
use claw_core::CompactionPolicy;
use claw_interface::{DiskFs, RealHttp, TokioTimer};
use claw_memory::{NoopCompactor, TranscriptConfig, TranscriptStore};

// The real network transport is `claw_interface::RealHttp` (the `realhttp`
// feature); summarisation is disabled via claw-memory's `NoopCompactor` (the
// `compactor-stub` feature).

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
/// Returned separately from the transport so callers that mint clients
/// themselves (e.g. [`claw_core::agent::FsAgentFactory`], which inits one client
/// per agent) can reuse this config.
///
/// # Panics
///
/// If any required `CLAW_LLM_*` variable is missing — there is no safe default
/// for these, so fail loudly.
pub fn make_llm_config() -> ClawApiConfig {
    let api_key = std::env::var("CLAW_LLM_API_KEY")
        .ok()
        .filter(|v| !v.is_empty())
        .expect("CLAW_LLM_API_KEY must be set");
    let base_url = std::env::var("CLAW_LLM_BASE_URL").expect("CLAW_LLM_BASE_URL must be set");
    let model = std::env::var("CLAW_LLM_MODEL").expect("CLAW_LLM_MODEL must be set");

    let mut config = ClawApiConfig::new(BackendKind::OpenAiCompatible, api_key, model, base_url);
    config.timeout_ms = 60_000;
    config
}

/// The live network transport ([`RealHttp`]). Each LLM client owns its own.
pub fn make_http() -> RealHttp {
    RealHttp::new()
}

/// Build a live LLM client from the `CLAW_LLM_*` environment variables.
///
/// # Panics
///
/// If any required `CLAW_LLM_*` variable is missing, or the client cannot init.
pub fn make_llm() -> ClawApiAsync<RealHttp, TokioTimer> {
    ClawApiAsync::init(make_llm_config(), make_http(), TokioTimer)
        .expect("failed to init LLM client")
}

/// The real disk storage backend the CLI runs its transcripts over. `DiskFs` is
/// a cheap clone handle (just a base path), so each agent gets its own clone.
pub fn make_storage() -> CliFs {
    DiskFs::absolute()
}

/// Build the compaction collaborators an agent's rolling-summary adapter needs:
/// the no-op compactor (summarisation is disabled in the CLI), and the default
/// compaction policy. These belong to the
/// agent layer, not the transcript store, which never compacts.
///
/// Public for CLIs that build a single [`claw_core::agent::GenericAgent`]
/// directly. [`claw_core::agent::FsAgentFactory`] owns this wiring internally.
pub fn make_compaction() -> CompactionDeps {
    CompactionDeps {
        compactor: Arc::new(NoopCompactor),
        policy: CompactionPolicy::new(6000, 2000, 1500),
    }
}

/// Build an on-disk [`TranscriptStore`] at `transcript_dir` plus a cloned read-only
/// view of the same store (handy for a `/messages` command). For agents that
/// build their own store (e.g. [`claw_core::agent::GenericAgent`]), use
/// [`make_memory_ingredients`] instead.
pub fn make_memory(
    agent_id: usize,
    transcript_dir: &str,
) -> (TranscriptStore<CliFs>, TranscriptStore<CliFs>) {
    let store = TranscriptStore::new(
        agent_id,
        TranscriptConfig::new(transcript_dir),
        make_storage(),
    );
    let view = store.clone();
    (store, view)
}

/// Build the ingredients a [`claw_core::agent::GenericAgent`] needs to construct
/// its own on-disk transcript store at `transcript_dir`: the transcript config, the
/// storage backend, and the compaction collaborators. The agent keys the
/// conversation by its own id, so no id is needed here.
pub fn make_memory_ingredients(transcript_dir: &str) -> (TranscriptConfig, CliFs, CompactionDeps) {
    (
        TranscriptConfig::new(transcript_dir),
        make_storage(),
        make_compaction(),
    )
}
