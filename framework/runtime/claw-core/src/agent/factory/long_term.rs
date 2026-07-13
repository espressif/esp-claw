use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use claw_interface::ClawFs;
use claw_memory::{LongTermInitError, LongTermMemory};

use crate::memory::{agent_store, global_store, Extractor};

use super::layout::join_storage_path;

const GLOBAL_LONG_TERM_DIR: &str = "global";
const AGENT_LONG_TERM_DIR: &str = "agents";

pub(super) struct LongTermDeps<F: ClawFs + 'static> {
    pub(super) global: LongTermMemory<F>,
    agents: AgentMemoryStores<F>,
    pub(super) extractor: Arc<dyn Extractor>,
}

struct AgentMemoryStores<F: ClawFs + 'static> {
    root_dir: String,
    by_kind: Mutex<BTreeMap<String, LongTermMemory<F>>>,
}

impl<F: ClawFs + 'static> AgentMemoryStores<F> {
    fn new(root_dir: String) -> Self {
        Self {
            root_dir,
            by_kind: Mutex::new(BTreeMap::new()),
        }
    }

    fn get(&self, kind: &str) -> Result<LongTermMemory<F>, LongTermInitError> {
        let mut stores = self
            .by_kind
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        if let Some(store) = stores.get(kind) {
            return Ok(store.clone());
        }

        let dir = join_storage_path(&self.root_dir, kind);
        let store = agent_store::<F>(&dir)?;
        stores.insert(kind.to_owned(), store.clone());
        Ok(store)
    }
}

impl<F: ClawFs + 'static> LongTermDeps<F> {
    pub(super) fn from_root(
        long_term_dir: &str,
        extractor: Arc<dyn Extractor>,
    ) -> Result<Self, LongTermInitError> {
        let global_dir = join_storage_path(long_term_dir, GLOBAL_LONG_TERM_DIR);
        let agent_root_dir = join_storage_path(long_term_dir, AGENT_LONG_TERM_DIR);
        Ok(Self {
            global: global_store::<F>(&global_dir)?,
            agents: AgentMemoryStores::new(agent_root_dir),
            extractor,
        })
    }

    pub(super) fn agent_store(&self, kind: &str) -> Result<LongTermMemory<F>, LongTermInitError> {
        self.agents.get(kind)
    }
}

#[cfg(test)]
mod tests {
    use claw_interface::MemFs;
    use claw_memory::{MemoryDraft, StoreOutcome};

    use super::AgentMemoryStores;

    #[test]
    fn same_kind_reuses_one_live_store_owner() {
        MemFs::new();
        let stores = AgentMemoryStores::<MemFs>::new("/memory/agents".to_owned());
        let first = stores.get("conversation").expect("first store opens");
        let second = stores.get("conversation").expect("second store opens");

        assert!(matches!(
            first.store(MemoryDraft::new("fact from first agent")),
            StoreOutcome::Created(_)
        ));
        assert_eq!(second.list().len(), 1);
    }
}
