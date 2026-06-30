//! Commands routed through the orchestrator.

use serde::{Deserialize, Serialize};

use super::{ApprovalRecord, TaskId};

/// Orchestrator command from user, frontend, or external controller.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Command {
    CreateTask {
        task_id: TaskId,
        goal: String,
        frontend_instance_id: String,
        #[serde(default)]
        requires_plan_approval: bool,
    },
    ApprovePlan {
        task_id: TaskId,
        approval: ApprovalRecord,
    },
    ApprovalResponse {
        task_id: TaskId,
        approval: ApprovalRecord,
    },
    RequestPlanRevision {
        task_id: TaskId,
        note: String,
    },
    CancelRun {
        run_id: String,
    },
}
