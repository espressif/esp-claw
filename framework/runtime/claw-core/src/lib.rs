//! `claw_core` — runtime primitives for the agent orchestrator.
//!
//! Layer 1: [`Orchestrator`]

pub mod agent;
// NOTE: the `memory` context-adapter surface is consumed only by in-repo dev
// tools/tests (the boundary `claw-agent` wires it entirely through
// `FsAgentFactory`), so it is a candidate for the same `dev`-gating as the agent
// concrete impls. That narrowing is deferred to the Batch F memory restructure
// (rename to `context-adapters`, move `Compactor`) to avoid churning it twice.
pub mod memory;
mod orchestrator;
mod session;

pub use agent::IterationId;
pub use claw_utils::{define_id_allocator, define_prefixed_id, IdParseError};
pub use memory::{
    agent_store, global_store, AssistantCommit, ContextAdapter, ContextAdapterInput, ExtractError,
    ExtractedItem, ExtractionInput, Extractor, History, LlmCompactor, LlmExtractor,
    LongTermMemoryContextAdapter, MemoryOp, MemorySnapshot, MemoryTier, MemoryTierHint,
    NoopExtractor, ProfileContextAdapter, ProfileTools, RuleBasedTierClassifier, TierClassifier,
    Transcript, AGENT_ID_PREFIX, GLOBAL_ID_PREFIX,
};
pub use orchestrator::{ApprovalRequest, DriveOutput, Orchestrator, RootReply};
pub use session::{
    DeliverError, DeliveryKind, FsSessionRegistry, SessionBinding, SessionError, SessionId,
    SessionMessage, SessionRecord, SessionRegistryStore, SessionStore,
};
