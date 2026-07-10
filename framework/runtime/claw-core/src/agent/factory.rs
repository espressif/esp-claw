//! Building an agent of a kind through the production [`FsAgentFactory`].
//!
//! [`FsAgentFactory`] turns a kind into a real, running
//! [`GenericAgent`](super::generic_agent::GenericAgent) by tying
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

mod construction;
mod create;
mod error;
mod layout;
mod long_term;

use std::marker::PhantomData;
use std::sync::Arc;

use claw_api::ClawApiConfig;
use claw_interface::{ClawFs, ClawHttp, ClawTimer};
use claw_memory::ProfileStore;
use claw_skill::FsSkillRegistry;
use claw_tool::ToolRegistry;

use self::long_term::LongTermDeps;

pub(crate) use error::{FsAgentCreateError, FsAgentFactoryError};
pub(crate) use layout::AgentPlacement;

/// Builds real [`GenericAgent`](super::generic_agent::GenericAgent)s from
/// compile-time manifests.
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
    /// shared global store, fronted by one `LongTermMemoryContextAdapter`.
    long_term: LongTermDeps<Filesystem>,
    /// Global editable profile documents, fronted by one `ProfileContextAdapter`
    /// per agent so file edits are observed on the next context build.
    profile: ProfileStore<Filesystem>,
    /// Shared skill catalog scanned from the configured roots. Cloned (an `Arc`
    /// bump) into every agent's `SkillSet` so all agents share one catalog and a
    /// reload is observed everywhere.
    skills: Arc<FsSkillRegistry<Filesystem>>,
}
