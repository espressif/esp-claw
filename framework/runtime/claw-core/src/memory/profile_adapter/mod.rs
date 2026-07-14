//! Profile context adapter: project editable profile documents into context.
//!
//! The store lives in `claw-memory`; this adapter is the agent-runtime layer that
//! maps documents to `BlockKind`s and optionally exposes profile-specific tools.

use claw_context::{Block, BlockKind, ContextSink};
use claw_interface::ClawFs;
use claw_memory::{ProfileDocument, ProfileStore};
use claw_tool::ToolGroup;

use crate::memory::traits::ContextAdapter;

mod tools;

use self::tools::profile_tools;

/// Whether this adapter exposes profile mutation tools to its agent.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ProfileTools {
    /// No profile tools. The adapter still contributes profile context.
    Disabled,
    /// Expose profile read/replace/clear tools.
    Writable,
}

/// Pulls global profile documents into the current agent context.
pub(crate) struct ProfileContextAdapter<F: ClawFs + 'static> {
    store: ProfileStore<F>,
    tools: ProfileTools,
}

impl<F: ClawFs + 'static> ProfileContextAdapter<F> {
    /// Build an adapter over `store`.
    pub(crate) fn new(store: ProfileStore<F>, writable: bool) -> Self {
        // Every agent receives profile context. Its owner explicitly decides
        // whether mutation tools are part of this agent's environment.
        Self {
            store,
            tools: if writable {
                ProfileTools::Writable
            } else {
                ProfileTools::Disabled
            },
        }
    }

    fn contribute_document(&self, document: ProfileDocument, output: &mut ContextSink<'_>) {
        let kind = match document {
            ProfileDocument::Soul => BlockKind::Soul,
            ProfileDocument::AssistantIdentity => BlockKind::AssistantIdentity,
            ProfileDocument::UserProfile => BlockKind::UserProfile,
        };
        match self.store.read(document) {
            Ok(Some(content)) => {
                output.block(Block::new(kind, content));
            }
            Ok(None) => {
                output.block(Block::new(kind, ""));
            }
            Err(error) => {
                tracing::warn!(
                    name: "profile_context_read_failed",
                    document = %document,
                    error = %error,
                );
            }
        }
    }
}

impl<F: ClawFs + 'static> ContextAdapter for ProfileContextAdapter<F> {
    fn contribute(&mut self, output: &mut ContextSink<'_>) {
        for document in ProfileDocument::all() {
            self.contribute_document(document, output);
        }
    }

    fn tools(&self) -> Option<ToolGroup> {
        match self.tools {
            ProfileTools::Disabled => None,
            ProfileTools::Writable => Some(profile_tools(self.store.clone())),
        }
    }
}
