//! Shared task contract between frontend and worker agents.

use serde::{Deserialize, Serialize};

use super::{StepId, TaskId, WorkerId};

/// Lifecycle status of a delegated task.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum TaskStatus {
    Draft,
    Active,
    Paused,
    Blocked,
    Done,
    Cancelled,
}

/// Immutable shared task description (no chat history).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct TaskContract {
    pub task_id: TaskId,
    pub run_id: String,
    pub goal: String,
    pub constraints: Vec<String>,
    pub env_manifest: String,
    pub status: TaskStatus,
    pub worker_instance_id: Option<WorkerId>,
    pub frontend_instance_id: String,
    /// When true, worker waits for [`super::Command::ApprovePlan`] before ACT.
    pub requires_plan_approval: bool,
}

impl TaskContract {
    pub fn new(
        task_id: TaskId,
        run_id: impl Into<String>,
        goal: impl Into<String>,
        frontend_instance_id: impl Into<String>,
        env_manifest: impl Into<String>,
    ) -> Self {
        Self {
            task_id,
            run_id: run_id.into(),
            goal: goal.into(),
            constraints: Vec::new(),
            env_manifest: env_manifest.into(),
            status: TaskStatus::Draft,
            worker_instance_id: None,
            frontend_instance_id: frontend_instance_id.into(),
            requires_plan_approval: false,
        }
    }
}

/// User approval decision on a worker plan or step.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ApprovalRecord {
    pub task_id: TaskId,
    pub step_id: Option<StepId>,
    pub approved: bool,
    pub note: Option<String>,
}
