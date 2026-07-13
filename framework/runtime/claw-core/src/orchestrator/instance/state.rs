use super::graph_state::GraphState;
use super::scheduler::SchedulerState;

#[derive(Default)]
pub(crate) struct OrchestratorInstanceState {
    pub(super) graph: GraphState,
    pub(super) scheduler: SchedulerState,
}
