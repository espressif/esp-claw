//! Shared scaffolding for the agent CLIs.
//!
//! The binaries in this crate drive real agents against a live LLM with on-disk
//! conversation memory. The platform dependencies are identical, so the real-disk
//! [`ClawFs`], live async HTTP, and the env/LLM/memory wiring live here once.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`.

use std::path::Path;

use claw_api::{BackendKind, ClawApiAsync, ClawApiConfig};
use claw_interface::{DiskFs, RealHttp, TokioTimer};
use claw_memory::TranscriptConfig;

// The real network transport is `claw_interface::RealHttp` (the `realhttp`
// feature). GenericAgent owns its internal conversation compaction wiring.

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

/// Build the ingredients a CLI needs to construct an on-disk transcript store at
/// `transcript_dir`: the transcript config and storage backend.
pub fn make_memory_ingredients(transcript_dir: &str) -> (TranscriptConfig, CliFs) {
    (TranscriptConfig::new(transcript_dir), make_storage())
}
