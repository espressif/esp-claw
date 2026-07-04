//! Profile context adapter: project editable profile documents into context.
//!
//! The store lives in `claw-memory`; this adapter is the agent-runtime layer that
//! maps documents to `BlockKind`s and optionally exposes profile-specific tools.

use claw_capability::Tool;
use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{ProfileDocument, ProfileStore};

use crate::memory::traits::{ContextAdapter, ContextAdapterInput};

mod tools;

use self::tools::profile_tools;

const PROFILE_ADAPTER_ID: &str = "profile";

/// Whether this adapter exposes profile mutation tools to its agent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ProfileTools {
    /// No profile tools. The adapter still contributes profile context.
    Disabled,
    /// Expose profile read/replace/clear tools.
    Writable,
}

/// Pulls global profile documents into the current agent context.
pub struct ProfileContextAdapter<F: ClawFs + Clone + 'static> {
    store: ProfileStore<F>,
    tools: ProfileTools,
}

impl<F: ClawFs + Clone + 'static> ProfileContextAdapter<F> {
    /// Build an adapter over `store`.
    pub fn new(store: ProfileStore<F>, tools: ProfileTools) -> Self {
        Self { store, tools }
    }

    fn contribute_document(&self, document: ProfileDocument, output: &mut ContextSink<'_>) {
        let kind = block_kind(document);
        match self.store.read(document) {
            Ok(Some(content)) => {
                output.block(Block::new(kind, content));
            }
            Ok(None) => {
                output.block(Block::new(kind, ""));
            }
            Err(error) => {
                tracing::warn!(%error, document = %document, "profile context read failed");
                output.block(Block::new(kind, ""));
            }
        }
    }
}

impl<F: ClawFs + Clone + 'static> ContextAdapter for ProfileContextAdapter<F> {
    fn id(&self) -> &str {
        PROFILE_ADAPTER_ID
    }

    fn contribute(&mut self, _input: ContextAdapterInput<'_>, output: &mut ContextSink<'_>) {
        for document in ProfileDocument::all() {
            self.contribute_document(document, output);
        }
    }

    fn tools(&self) -> Vec<Tool> {
        match self.tools {
            ProfileTools::Disabled => Vec::new(),
            ProfileTools::Writable => profile_tools(self.store.clone()),
        }
    }
}

fn block_kind(document: ProfileDocument) -> BlockKind {
    match document {
        ProfileDocument::Soul => BlockKind::Soul,
        ProfileDocument::AssistantIdentity => BlockKind::AssistantIdentity,
        ProfileDocument::UserProfile => BlockKind::UserProfile,
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use std::sync::Arc;

    use claw_context::Context;
    use claw_interface::MemFs;
    use claw_memory::ProfileConfig;
    use serde_json::Value;

    use super::*;
    use crate::memory::History;

    struct EmptyHistory;

    impl History for EmptyHistory {
        fn messages(&self) -> Arc<Value> {
            Arc::new(Value::Array(vec![]))
        }

        fn version(&self) -> u64 {
            0
        }
    }

    fn system_of(context: &mut Context) -> String {
        let history = Value::Array(vec![]);
        context.request(&history).system().to_string()
    }

    #[test]
    fn contributes_profile_documents_in_context_order() {
        let fs = MemFs::new();
        let store = ProfileStore::new(ProfileConfig::new("/memory"), fs);
        store.replace(ProfileDocument::UserProfile, "USER").unwrap();
        store.replace(ProfileDocument::Soul, "SOUL").unwrap();
        store
            .replace(ProfileDocument::AssistantIdentity, "IDENTITY")
            .unwrap();

        let mut adapter = ProfileContextAdapter::new(store, ProfileTools::Disabled);
        let mut context = Context::new();
        let history = EmptyHistory;
        let mut sink = context.sink();
        adapter.contribute(ContextAdapterInput { history: &history }, &mut sink);
        drop(sink);

        assert_eq!(system_of(&mut context), "SOUL\n\nIDENTITY\n\nUSER");
    }

    #[test]
    fn missing_document_clears_existing_block() {
        let fs = MemFs::new();
        let store = ProfileStore::new(ProfileConfig::new("/memory"), fs);
        let mut adapter = ProfileContextAdapter::new(store, ProfileTools::Disabled);
        let mut context = Context::new();
        context.with(Block::new(BlockKind::Soul, "OLD"));
        let history = EmptyHistory;
        let mut sink = context.sink();
        adapter.contribute(ContextAdapterInput { history: &history }, &mut sink);
        drop(sink);

        assert_eq!(system_of(&mut context), "");
    }
}
