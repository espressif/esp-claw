//! Transcript access and context adapters used by the agent loop.
//!
//! Storage lives in `claw-memory`; this module owns its LLM-backed projections
//! and tool wiring.

mod async_llm;
mod conversation_history_adapter;
mod long_term_memory_adapter;
mod profile_adapter;
mod skill_adapter;
mod traits;

pub(crate) use conversation_history_adapter::{
    CompactionPolicy, ConversationHistoryContextAdapter, LlmCompactor,
};
pub(crate) use long_term_memory_adapter::{
    agent_store, global_store, Extractor, LlmExtractor, LongTermMemoryContextAdapter,
};
pub(crate) use profile_adapter::ProfileContextAdapter;
pub(crate) use skill_adapter::SkillContextAdapter;
pub(crate) use traits::{AssistantCommit, ContextAdapter, History, Transcript};
