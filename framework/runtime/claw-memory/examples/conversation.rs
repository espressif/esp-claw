//! Drive a [`TranscriptStore`] through a few turns and inspect what the model
//! would see.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-memory --example conversation --target x86_64-unknown-linux-gnu
//! ```
//!
//! The store is pure storage — no summarization, no LLM. Persistence is an
//! in-memory [`MemFs`]; on device the same code runs over the DATA root.

use claw_interface::MemFs;
use claw_memory::{TranscriptConfig, TranscriptStore};

fn main() -> anyhow::Result<()> {
    let conversation_id = 42;
    MemFs::new();
    let store = TranscriptStore::<MemFs>::new(
        conversation_id,
        TranscriptConfig::new("/data/conversations"),
    )?;

    // One turn = one `group()`. The whole turn commits as a single record when
    // the guard drops at the end of the scope.
    {
        let turn = store.group();
        turn.append_user("what's the weather in Shanghai?");
        turn.append_assistant(r#"{"role":"assistant","content":"Let me check."}"#);
        turn.append_tool_result("call_1", r#"{"temp_c":21,"sky":"clear"}"#, false);
    }

    {
        let turn = store.group();
        turn.append_user("and tomorrow?");
        turn.append_assistant(r#"{"role":"assistant","content":"Sunny, around 23C."}"#);
    }

    // `messages()` is what you feed to the model — the full verbatim transcript,
    // committed turns plus any open one.
    let messages = store.messages();
    let count = messages.as_array().map_or(0, Vec::len);
    println!("conversation has {count} message(s) to send to the model:\n");
    println!("{}", serde_json::to_string_pretty(&*messages)?);

    // Force a checkpoint, e.g. on a clean shutdown.
    store.flush();
    Ok(())
}
