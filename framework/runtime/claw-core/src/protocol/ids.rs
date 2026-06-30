//! Strongly typed protocol identifiers (except [`crate::iteration_loop::IterationId`]).
//!
//! In memory these are numeric (`usize`). On the wire (JSON and [`Display`]) they use
//! prefixed strings: `session-1`, `task-1`, `step-1`, `worker-1`.

pub use claw_utils::IdParseError;

crate::define_prefixed_id!(TaskId, "task-", "task");
crate::define_prefixed_id!(StepId, "step-", "step");
crate::define_prefixed_id!(WorkerId, "worker-", "worker");

impl WorkerId {
    /// Default mapping: one worker instance per task, same numeric suffix.
    pub fn for_task(task_id: TaskId) -> Self {
        Self(task_id.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn task_id_serializes_to_prefixed_string() {
        let value = serde_json::to_value(TaskId(42)).unwrap();
        assert_eq!(value, json!("task-42"));
    }

    #[test]
    fn step_id_serializes_to_prefixed_string() {
        let value = serde_json::to_value(StepId(3)).unwrap();
        assert_eq!(value, json!("step-3"));
    }

    #[test]
    fn worker_id_serializes_to_prefixed_string() {
        let value = serde_json::to_value(WorkerId(5)).unwrap();
        assert_eq!(value, json!("worker-5"));
    }

    #[test]
    fn ids_deserialize_from_prefixed_string() {
        let task: TaskId = serde_json::from_value(json!("task-3")).unwrap();
        let step: StepId = serde_json::from_value(json!("step-2")).unwrap();
        let worker: WorkerId = serde_json::from_value(json!("worker-9")).unwrap();
        assert_eq!(task, TaskId(3));
        assert_eq!(step, StepId(2));
        assert_eq!(worker, WorkerId(9));
        assert_eq!(WorkerId::for_task(TaskId(3)), WorkerId(3));
    }

    #[test]
    fn ids_reject_non_prefixed_wire_values() {
        assert!(serde_json::from_value::<TaskId>(json!(3)).is_err());
        assert!(serde_json::from_value::<StepId>(json!("S1")).is_err());
        assert!(StepId::from_wire("step-").is_err());
    }

    #[test]
    fn command_roundtrip_uses_wire_ids() {
        use crate::protocol::Command;

        let command = Command::CreateTask {
            task_id: TaskId(1),
            goal: "build".into(),
            frontend_instance_id: "fe-1".into(),
            requires_plan_approval: false,
        };
        let value = serde_json::to_value(&command).unwrap();
        assert_eq!(value["CreateTask"]["task_id"], json!("task-1"));

        let restored: Command = serde_json::from_value(value).unwrap();
        assert_eq!(
            restored,
            Command::CreateTask {
                task_id: TaskId(1),
                goal: "build".into(),
                frontend_instance_id: "fe-1".into(),
                requires_plan_approval: false,
            }
        );
    }

    #[test]
    fn display_matches_wire_format() {
        assert_eq!(TaskId(1).to_string(), "task-1");
        assert_eq!(StepId(1).to_string(), "step-1");
        assert_eq!(WorkerId(1).to_string(), "worker-1");
    }
}
