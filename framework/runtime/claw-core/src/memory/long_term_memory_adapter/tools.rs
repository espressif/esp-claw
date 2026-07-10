//! Model-callable long-term memory tools over the dual-tier store.

mod args;
mod handlers;

use claw_interface::ClawFs;
use claw_tool::Tool;

use super::MemoryStores;

pub(crate) fn memory_tools<F: ClawFs + 'static>(stores: MemoryStores<F>) -> Vec<Tool> {
    handlers::memory_tools(stores)
}
