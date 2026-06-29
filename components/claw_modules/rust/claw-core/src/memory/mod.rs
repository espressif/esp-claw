//! Memory wiring: the agent-layer memory subsystem.
//!
//! `claw-memory` (the core crate) defines only the storage and seams — the
//! `Compactor` trait, the [`ConversationMemory`](claw_memory::ConversationMemory)
//! tape, and the [`LongTermMemory`](claw_memory::LongTermMemory) store — and never
//! the LLM-backed policies. This module is the agent wiring layer's home for
//! everything that needs the LLM client or ties memory into the agent loop:
//!
//! - the [`History`] / [`Transcript`] / [`Memory`] traits the agent reads, writes,
//!   and pulls memories through;
//! - the [`ConversationHistory`] that owns the transcript;
//! - the LLM-backed [`LlmCompactor`] and [`LlmExtractor`];
//! - the [`Extractor`] seam and tier policy ([`TierClassifier`]);
//! - the [`LongTermMemoryAdapter`] that fronts the dual-tier store as one
//!   [`Memory`] with its five model-callable tools.

mod conversation_history;
mod extraction;
mod llm_compactor;
mod llm_extractor;
mod long_term_adapter;
mod tier;
mod tools;
mod traits;

pub use conversation_history::ConversationHistory;
pub use extraction::{ExtractError, ExtractedItem, Extractor, NoopExtractor};
pub use llm_compactor::LlmCompactor;
pub use llm_extractor::LlmExtractor;
pub use long_term_adapter::{
    agent_store, global_store, LongTermMemoryAdapter, AGENT_ID_PREFIX, GLOBAL_ID_PREFIX,
};
pub use tier::{MemoryTier, RuleBasedTierClassifier, TierClassifier};
pub use traits::{History, Memory, Transcript};
