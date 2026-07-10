//! `claw-memory` — the agent memory subsystem.
//!
//! Two stores live here, both pure storage:
//!
//! - [`TranscriptStore`] — the complete, append-only verbatim transcript
//!   (the source of truth for what was said).
//! - [`ProfileStore`] — the editable global profile documents (`soul.md`,
//!   `identity.md`, `user.md`).
//! - [`LongTermMemory`] — the durable fact store.
//!
//! As a core crate it depends only on the [`claw_interface`] inbound traits — the
//! [`ClawFs`](claw_interface::ClawFs) persistence seam — never on the platform
//! boundary (`claw-sys`) or on the LLM client (`claw-api`).
//!
//! # Compaction is *not* here
//!
//! Folding an aged transcript prefix into a summary so it fits the model's
//! context window is a property of the **LLM request**, not of the stored record.
//! The [`TranscriptStore`] therefore never summarizes or deletes turns; it just
//! stores them. This crate only defines the [`Compactor`] *seam* — the
//! transformation "turn a window of messages into a summary" — which the agent
//! layer's rolling-summary context adapter (in `claw_core`) owns and drives. The
//! ready-made LLM-backed compactor (`LlmCompactor`) lives in `claw_core`, which
//! has the LLM client.
//!
//! # Using the transcript store
//!
//! ```no_run
//! use claw_interface::MemFs;
//! use claw_memory::TranscriptStore;
//!
//! // A filesystem for persistence. On device this is the espidf `ClawFs` over
//! // the DATA root; here it is the in-memory host double. The store holds the
//! // type parameter `F`.
//! MemFs::new();
//!
//! // Build the store for one transcript id. Typically one per agent instance.
//! let transcript_id = 42;
//! let mut store = TranscriptStore::<MemFs>::new(transcript_id, "/data/transcripts")
//!     .expect("a fresh MemFs has no data log, so the transcript starts empty");
//!
//! // Drive it from the agent loop. One turn = one `group()`; the whole turn is
//! // committed as a single record when the guard drops.
//! {
//!     let turn = store.group();
//!     turn.append_user("what's the weather?");
//!     turn.append_assistant(r#"{"role":"assistant","content":"Sunny."}"#);
//!     turn.append_tool_result("call_1", "{\"temp_c\":21}", false);
//!
//!     // store.messages() includes the open turn; feed to the model.
//!     let messages = store.messages();
//!     let _ = messages;
//! } // drop → the turn is committed
//!
//! store.flush(); // force a checkpoint, e.g. on a clean shutdown
//! ```

pub mod compaction;
pub mod long_term_memory;
pub mod profile;
pub mod transcript_store;

#[cfg(feature = "compactor-stub")]
pub use compaction::NoopCompactor;
pub use compaction::{CompactBackendError, CompactError, CompactFuture, Compactor};
pub use long_term_memory::{
    LongTermError, LongTermInitError, LongTermMemory, MemoryDraft, MemoryId, MemoryItem,
    MemoryPatch, StoreOutcome,
};
pub use profile::{
    ParseProfileDocumentError, ProfileDocument, ProfileError, ProfileSnapshot, ProfileStore,
    ASSISTANT_IDENTITY_FILE, DEFAULT_PROFILE_DOCUMENT_MAX_BYTES, SOUL_FILE, USER_PROFILE_FILE,
};
pub use transcript_store::{GroupGuard, TranscriptInitError, TranscriptStore, Turn, TurnId};
