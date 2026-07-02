//! `claw_core` — runtime primitives for the agent orchestrator.
//!
//! Layer 1: [`Orchestrator`]

#![allow(non_camel_case_types)]

pub mod agent;
pub mod memory;
mod orchestrator;
mod session;

pub use agent::IterationId;
pub use claw_utils::{define_prefixed_id, IdParseError};
pub use memory::{
    agent_store, global_store, CompactionPolicy, ContextAdapter, ContextAdapterInput, ExtractError,
    ExtractedItem, Extractor, History, LlmCompactor, LlmExtractor, LongTermMemoryContextAdapter,
    MemoryTier, NoopExtractor, ProfileContextAdapter, ProfileTools, RuleBasedTierClassifier,
    TierClassifier, Transcript, AGENT_ID_PREFIX, GLOBAL_ID_PREFIX,
};
pub use orchestrator::{ApprovalRequest, DriveOutput, Orchestrator, RootReply};
pub use session::{
    DeliverError, SessionError, SessionId, SessionMessage, SessionRecord, SessionStore,
};
