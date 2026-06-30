//! Memory wiring: the agent-layer memory subsystem.
//!
//! `claw-memory` (the core crate) defines only the storage and seams — the
//! `Compactor` trait, the [`TranscriptStore`](claw_memory::TranscriptStore), and
//! the [`LongTermMemory`](claw_memory::LongTermMemory) store — and never the
//! LLM-backed policies or compaction itself. This module is the agent wiring
//! layer's home for everything that needs the LLM client or ties memory into the
//! agent loop:
//!
//! - the [`History`] / [`Transcript`] / [`ContextAdapter`] traits the agent reads,
//!   writes, and pulls context sources through;
//! - the [`History`] / [`Transcript`] implementation over
//!   [`TranscriptStore`](claw_memory::TranscriptStore);
//! - the LLM-backed [`LlmCompactor`] and [`LlmExtractor`];
//! - the [`Extractor`] seam and tier policy ([`TierClassifier`]);
//! - the [`LongTermMemoryContextAdapter`] that fronts the dual-tier store as one
//!   [`ContextAdapter`] with its five model-callable tools.

mod long_term_memory_adapter;
mod recent_messages_adapter;
mod rolling_summary_adapter;
mod summary_cursor;
mod traits;

pub use long_term_memory_adapter::{
    agent_store, global_store, ExtractError, ExtractedItem, Extractor, LlmCompactor, LlmExtractor,
    LongTermMemoryContextAdapter, MemoryTier, NoopExtractor, RuleBasedTierClassifier,
    TierClassifier, AGENT_ID_PREFIX, GLOBAL_ID_PREFIX,
};
pub use recent_messages_adapter::RecentMessagesContextAdapter;
pub use rolling_summary_adapter::{CompactionPolicy, RollingSummaryContextAdapter};
pub use summary_cursor::SummaryCursor;
pub use traits::{ContextAdapter, History, Transcript};
