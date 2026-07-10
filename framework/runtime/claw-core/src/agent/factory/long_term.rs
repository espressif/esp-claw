use std::sync::Arc;

use claw_interface::ClawFs;
use claw_memory::{LongTermInitError, LongTermMemory};

use crate::memory::{global_store, Extractor, TierClassifier};

use super::layout::join_storage_path;

const GLOBAL_LONG_TERM_DIR: &str = "global";
const AGENT_LONG_TERM_DIR: &str = "agents";

/// The long-term-memory collaborators shared across every agent a factory
/// builds: the one global store, the derived per-agent-kind store root, and the
/// routing/extraction policies.
///
/// Built once by [`super::FsAgentFactory::new`]; each agent then gets its own
/// private store under `<long_term_dir>/agents/<kind>` plus a clone of the shared
/// global store under `<long_term_dir>/global`, fronted by one
/// `LongTermMemoryContextAdapter`.
pub(super) struct LongTermDeps<F: ClawFs + 'static> {
    /// The single store shared by every agent (user-level facts). Cloned (an
    /// `Arc` bump) into each agent's adapter so all agents read/write one store.
    pub(super) global: LongTermMemory<F>,
    /// Root under which each baked agent kind owns a private store directory.
    pub(super) agent_root_dir: String,
    /// Routes a new fact to the global or per-agent tier.
    pub(super) classifier: Arc<dyn TierClassifier>,
    /// Distills durable facts from the transcript.
    pub(super) extractor: Arc<dyn Extractor>,
}

impl<F: ClawFs + 'static> LongTermDeps<F> {
    /// Build the shared long-term collaborators from the explicit long-term
    /// memory root. The Rust memory runtime owns the internal layout below that
    /// root: `global` for shared facts and `agents/<kind>` for each baked agent
    /// kind's private memory.
    pub(super) fn from_root(
        long_term_dir: &str,
        classifier: Arc<dyn TierClassifier>,
        extractor: Arc<dyn Extractor>,
    ) -> Result<Self, LongTermInitError> {
        let global_dir = join_storage_path(long_term_dir, GLOBAL_LONG_TERM_DIR);
        let agent_root_dir = join_storage_path(long_term_dir, AGENT_LONG_TERM_DIR);
        Ok(Self {
            global: global_store::<F>(&global_dir)?,
            agent_root_dir,
            classifier,
            extractor,
        })
    }
}
