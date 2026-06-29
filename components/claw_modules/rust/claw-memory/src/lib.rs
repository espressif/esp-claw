//! `claw-memory` — the agent memory subsystem.
//!
//! Today this holds the short-term conversation memory ([`ConversationMemory`]).
//! Its background jobs (compaction now; profile / long-term extraction later) run
//! on the process-wide [`SharedTaskPool`](claw_utils::SharedTaskPool) — owned by
//! `claw-utils` so non-memory subsystems can share one pool — injected through
//! [`ConversationDeps`].
//!
//! As a core crate it depends only on the [`claw_interface`] inbound traits — the
//! [`ClawFs`](claw_interface::ClawFs) persistence seam — and on `claw-utils`,
//! never on the platform boundary (`claw-sys`) or on the LLM client (`claw-api`).
//! The pool, filesystem, and summarizer are all **injected** by the caller: the
//! summarization policy comes in through the [`Compactor`] trait, so this crate
//! owns the compaction *mechanism* but not the *transformation*. The ready-made
//! LLM-backed compactor (`LlmCompactor`) lives in `claw_core`, which has the LLM
//! client; wire it (or your own [`Compactor`]) into [`ConversationDeps`].
//!
//! # Using the conversation memory
//!
//! ```no_run
//! use std::sync::Arc;
//!
//! use claw_interface::{MemFs, StdThread};
//! use claw_memory::{
//!     CompactError, Compactor, ConversationConfig, ConversationDeps, ConversationMemory,
//! };
//! use claw_utils::{SharedTaskPool, PoolConfig};
//! use serde_json::{json, Value};
//!
//! // 1. A filesystem for persistence. On device this is the espidf `ClawFs` over
//! //    the DATA root; here it is the in-memory host double. A `ClawFs` hands out
//! //    `ClawFile` handles; the memory layer holds the type parameter `F`.
//! let fs = MemFs::new();
//!
//! // 2. A compactor that folds an aged window into a summary (e.g. an LLM call).
//! struct MyCompactor;
//! impl Compactor for MyCompactor {
//!     fn compact(&self, window: &[Value]) -> Result<Vec<Value>, CompactError> {
//!         Ok(vec![json!({
//!             "role": "system",
//!             "content": format!("summary of {} earlier messages", window.len()),
//!         })])
//!     }
//! }
//!
//! // 3. One pool is shared across the system — create it once at boot. The
//! //    caller injects the spawn policy (`StdThread` here; `EspIdfThread` on device).
//! let pool = Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread)?);
//!
//! // 4. Build the conversation memory for one conversation id. Typically one
//! //    memory per agent instance; all of them share the single pool above.
//! let conversation_id = 42;
//! let mut memory = ConversationMemory::new(
//!     conversation_id,
//!     ConversationConfig::new("/data/conversations"),
//!     ConversationDeps {
//!         fs,
//!         pool: Arc::clone(&pool),
//!         compactor: Arc::new(MyCompactor),
//!     },
//! );
//!
//! // 5. Drive it from the agent loop. One turn = one `group()`; the whole turn
//! //    is committed as a single record when the guard drops.
//! {
//!     let turn = memory.group();
//!     turn.append_user("what's the weather?");
//!     turn.append_assistant(r#"{"role":"assistant","content":"Sunny."}"#);
//!     turn.append_tool_result("call_1", "{\"temp_c\":21}", false);
//!
//!     // memory.messages() includes the open turn; feed to the model.
//!     let messages = memory.messages();
//!     let _ = messages;
//! } // drop → the turn is committed
//!
//! memory.flush(); // force a checkpoint, e.g. on a clean shutdown
//! # Ok::<(), std::io::Error>(())
//! ```
//!
//! Compaction is automatic and invisible: when committing a turn grows the
//! transcript past [`ConversationConfig::compact_threshold_tokens`], the memory
//! schedules a background job on the pool that calls your [`Compactor`] and
//! splices in the result. Callers never trigger it.

pub mod compaction;
pub mod conversation_memory;
pub mod long_term_memory;

#[cfg(feature = "compactor-stub")]
pub use compaction::NoopCompactor;
pub use compaction::{CompactError, Compactor};
pub use conversation_memory::{
    ConversationConfig, ConversationDeps, ConversationMemory, GroupGuard,
};
pub use long_term_memory::{
    LongTermConfig, LongTermError, LongTermMemory, MemoryDraft, MemoryId, MemoryItem, MemoryPatch,
    StoreOutcome,
};
