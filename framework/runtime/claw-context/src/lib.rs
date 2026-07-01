//! `claw-context` — the agent's context assembler.
//!
//! This crate owns **placement, change detection, and rendering**, never
//! **content**. A [`Context`] holds one block per [`BlockKind`], caches the
//! rendered system prefix, and pairs it with the message tail (history plus
//! ephemeral reminders) into a [`RequestContext`] for the LLM client. What each
//! block *contains* — instructions, persona, memory, retrieved knowledge — is
//! supplied by callers.
//!
//! # Model
//!
//! Declare blocks with [`Context::with`]; it is incremental and safe — a kind you
//! do not declare keeps its last value, empty content removes a kind, and
//! re-declaring identical content is a free no-op. Call [`Context::with`] every
//! tick for anything that might change (so the context is never stale without
//! betting that you "remembered" to push), and read with [`Context::request`],
//! which re-renders only when a block actually changed.
//!
//! # Example
//!
//! ```
//! use claw_context::{Block, BlockKind, Context};
//! use serde_json::json;
//!
//! let mut context = Context::new();
//! // Blocks may be declared in any order; the wire order is fixed by BlockKind.
//! context
//!     .with(Block::new(BlockKind::AgentInstruction, "You are a helpful agent."))
//!     .with(Block::new(BlockKind::OutputContract, "Answer in one concise paragraph."));
//!
//! let history = json!([{ "role": "user", "content": "What's the weather?" }]);
//! let request = context.request(&history);
//! assert_eq!(
//!     request.system(),
//!     "You are a helpful agent.\n\nAnswer in one concise paragraph."
//! );
//! ```

mod block;
mod context;
mod reminder;

pub use block::{Band, Block, BlockKind, Scope};
pub use context::{Context, ContextItem, ContextSink, RequestContext};
