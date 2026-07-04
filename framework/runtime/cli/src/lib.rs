//! Shared scaffolding for the agent CLIs.
//!
//! The binaries in this crate drive the orchestrator against a live LLM with
//! on-disk memory. The real-disk [`ClawFs`] backend and env/LLM config wiring
//! live here once.
//!
//! LLM config is read from `claw-core/.env.local` (the same file the integration
//! tests use): `CLAW_LLM_API_KEY`, `CLAW_LLM_BASE_URL`, `CLAW_LLM_MODEL`.

use std::path::Path;

use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::DiskFs;

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
/// Returned separately from the transport so the orchestrator can mint its own
/// per-agent clients.
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
