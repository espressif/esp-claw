//! Events emitted by agents and routed by the orchestrator.

use serde::{Deserialize, Serialize};

use super::{StepId, TaskId, WorkerId};

/// Agent-to-agent or agent-to-frontend event.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub enum AgentEvent {
    PlanProposed {
        task_id: TaskId,
        worker_instance_id: Option<WorkerId>,
        steps: Vec<StepId>,
    },
    Progress {
        task_id: TaskId,
        worker_instance_id: Option<WorkerId>,
        step_id: StepId,
        message: String,
    },
    ApprovalRequested {
        task_id: TaskId,
        worker_instance_id: Option<WorkerId>,
        step_id: StepId,
        reason: String,
    },
    Blocked {
        task_id: TaskId,
        worker_instance_id: Option<WorkerId>,
        step_id: Option<StepId>,
        reason: String,
    },
    Done {
        task_id: TaskId,
        worker_instance_id: Option<WorkerId>,
        summary: String,
    },
}

impl AgentEvent {
    pub fn task_id(&self) -> TaskId {
        match self {
            AgentEvent::PlanProposed { task_id, .. }
            | AgentEvent::Progress { task_id, .. }
            | AgentEvent::ApprovalRequested { task_id, .. }
            | AgentEvent::Blocked { task_id, .. }
            | AgentEvent::Done { task_id, .. } => *task_id,
        }
    }

    pub fn target_frontend_hint(&self) -> Option<&str> {
        None
    }
}
