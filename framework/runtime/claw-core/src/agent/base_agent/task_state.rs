use std::collections::VecDeque;

use serde::{Deserialize, Serialize};

use crate::agent::tools::ControlSignal;
use crate::session::Message;

use super::{AgentCommand, AgentCommandError, AgentState, ApprovalDecision};

#[derive(Deserialize, Serialize)]
enum TaskPhase {
    Idle,
    Running,
    AwaitingApproval(Vec<String>),
}

#[derive(Clone, Copy)]
enum TaskPhaseView {
    Idle,
    Running,
    AwaitingApproval,
}

impl TaskPhase {
    fn view(&self) -> TaskPhaseView {
        match self {
            Self::Idle => TaskPhaseView::Idle,
            Self::Running => TaskPhaseView::Running,
            Self::AwaitingApproval(_) => TaskPhaseView::AwaitingApproval,
        }
    }
}

impl TaskPhaseView {
    fn public(self) -> AgentState {
        match self {
            Self::Idle => AgentState::Idle,
            Self::Running => AgentState::Running,
            Self::AwaitingApproval => AgentState::AwaitingApproval,
        }
    }
}

#[derive(Deserialize, Serialize)]
enum Inbound {
    Command(AgentCommand),
    TaskInput(#[serde(deserialize_with = "crate::session::deserialize_message_or_text")] Message),
    Control(ControlSignal),
}

#[derive(Deserialize, Serialize)]
struct TaskMailbox {
    entries: VecDeque<Inbound>,
}

impl TaskMailbox {
    fn new() -> Self {
        Self {
            entries: VecDeque::new(),
        }
    }

    fn enqueue(&mut self, inbound: Inbound) {
        self.entries.push_back(inbound);
    }

    fn pop_front(&mut self) -> Option<Inbound> {
        self.entries.pop_front()
    }

    fn projected_phase(&self, committed: &TaskPhase) -> Result<TaskPhaseView, AgentCommandError> {
        self.entries
            .iter()
            .try_fold(committed.view(), |phase, inbound| {
                transition(phase, inbound)
            })
    }
}

pub(super) enum TaskAction {
    TaskInput {
        message: Message,
        starts_task: bool,
    },
    Cancel,
    ApprovalResult {
        decision: ApprovalDecision,
        grant_signatures: Vec<String>,
    },
    EndConversation {
        final_message: String,
    },
}

#[derive(Debug, thiserror::Error)]
pub(super) enum TaskStateError {
    #[error("cannot await approval while the task is {state:?}")]
    CannotAwaitApproval { state: AgentState },
    #[error("cannot await approval while task inputs are still queued")]
    PendingMailbox,
}

/// Owns the one committed task phase and all accepted-but-unapplied inputs.
#[derive(Deserialize, Serialize)]
pub(super) struct TaskState {
    phase: TaskPhase,
    mailbox: TaskMailbox,
}

impl TaskState {
    pub(super) fn new() -> Self {
        Self {
            phase: TaskPhase::Idle,
            mailbox: TaskMailbox::new(),
        }
    }

    pub(super) fn enqueue_command(
        &mut self,
        command: AgentCommand,
    ) -> Result<(), AgentCommandError> {
        let projected = self.mailbox.projected_phase(&self.phase)?;
        classify(projected, &command)?;
        self.mailbox.enqueue(Inbound::Command(command));
        Ok(())
    }

    pub(super) fn enqueue_task_input(&mut self, message: Message) {
        self.mailbox.enqueue(Inbound::TaskInput(message));
    }

    pub(super) fn enqueue_control(&mut self, signal: ControlSignal) {
        self.mailbox.enqueue(Inbound::Control(signal));
    }

    pub(super) fn pop_action(&mut self) -> Result<Option<TaskAction>, AgentCommandError> {
        let current = self.phase.view();
        let Some(inbound) = self.mailbox.pop_front() else {
            return Ok(None);
        };
        transition(current, &inbound)?;

        let action = match inbound {
            Inbound::Command(AgentCommand::AppendMessage(message))
            | Inbound::TaskInput(message) => {
                let starts_task = matches!(current, TaskPhaseView::Idle);
                if starts_task {
                    self.phase = TaskPhase::Running;
                }
                TaskAction::TaskInput {
                    message,
                    starts_task,
                }
            }
            Inbound::Command(AgentCommand::Cancel) => {
                self.phase = TaskPhase::Idle;
                TaskAction::Cancel
            }
            Inbound::Command(AgentCommand::ApprovalResult(decision)) => {
                let previous = std::mem::replace(&mut self.phase, TaskPhase::Running);
                let grant_signatures = match previous {
                    TaskPhase::AwaitingApproval(signatures) => signatures,
                    TaskPhase::Idle => {
                        return Err(AgentCommandError::NotAwaitingApproval {
                            state: AgentState::Idle,
                        });
                    }
                    TaskPhase::Running => {
                        return Err(AgentCommandError::NotAwaitingApproval {
                            state: AgentState::Running,
                        });
                    }
                };
                TaskAction::ApprovalResult {
                    decision,
                    grant_signatures,
                }
            }
            Inbound::Control(ControlSignal::EndConversation { final_message }) => {
                self.phase = TaskPhase::Idle;
                TaskAction::EndConversation { final_message }
            }
        };
        Ok(Some(action))
    }

    pub(super) fn await_approval(
        &mut self,
        grant_signatures: Vec<String>,
    ) -> Result<(), TaskStateError> {
        if !self.mailbox.entries.is_empty() {
            return Err(TaskStateError::PendingMailbox);
        }
        if !matches!(self.phase, TaskPhase::Running) {
            return Err(TaskStateError::CannotAwaitApproval {
                state: self.phase.view().public(),
            });
        }
        self.phase = TaskPhase::AwaitingApproval(grant_signatures);
        Ok(())
    }

    pub(super) fn finish_task(&mut self) {
        self.phase = TaskPhase::Idle;
    }

    pub(super) fn is_running(&self) -> bool {
        matches!(self.phase, TaskPhase::Running)
    }

    pub(super) fn validate(&self) -> Result<(), AgentCommandError> {
        self.mailbox.projected_phase(&self.phase).map(|_| ())
    }
}

fn transition(phase: TaskPhaseView, inbound: &Inbound) -> Result<TaskPhaseView, AgentCommandError> {
    match inbound {
        Inbound::Command(command) => classify(phase, command),
        Inbound::TaskInput(_) => match phase {
            TaskPhaseView::Idle => Ok(TaskPhaseView::Running),
            TaskPhaseView::Running | TaskPhaseView::AwaitingApproval => Ok(phase),
        },
        Inbound::Control(ControlSignal::EndConversation { .. }) => Ok(TaskPhaseView::Idle),
    }
}

fn classify(
    phase: TaskPhaseView,
    command: &AgentCommand,
) -> Result<TaskPhaseView, AgentCommandError> {
    use AgentCommand as Command;
    use TaskPhaseView as Phase;

    match (phase, command) {
        (Phase::Idle, Command::AppendMessage(_)) => Ok(Phase::Running),
        (phase @ (Phase::Running | Phase::AwaitingApproval), Command::AppendMessage(_)) => {
            Err(AgentCommandError::CannotAppend {
                state: phase.public(),
            })
        }
        (Phase::Idle, Command::Cancel) => Err(AgentCommandError::NothingToCancel),
        (Phase::Running | Phase::AwaitingApproval, Command::Cancel) => Ok(Phase::Idle),
        (Phase::AwaitingApproval, Command::ApprovalResult(_)) => Ok(Phase::Running),
        (phase @ (Phase::Idle | Phase::Running), Command::ApprovalResult(_)) => {
            Err(AgentCommandError::NotAwaitingApproval {
                state: phase.public(),
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use crate::session::Message;

    use super::*;

    fn projected_state(task: &TaskState) -> Result<AgentState, AgentCommandError> {
        task.mailbox
            .projected_phase(&task.phase)
            .map(TaskPhaseView::public)
    }

    #[test]
    fn task_input_preserves_the_message_until_reduction() {
        let mut task = TaskState::new();
        let input = Message::text("hello");

        task.enqueue_task_input(input.clone());

        assert!(matches!(
            task.pop_action().expect("valid task input"),
            Some(TaskAction::TaskInput {
                message,
                starts_task: true,
            }) if message == input
        ));
    }

    #[test]
    fn mailbox_validates_against_queued_transitions_without_a_second_phase_field() {
        let mut task = TaskState::new();
        task.enqueue_command(AgentCommand::AppendMessage(Message::text("first")))
            .expect("the idle task accepts its first message");

        assert!(matches!(task.phase, TaskPhase::Idle));
        assert_eq!(projected_state(&task), Ok(AgentState::Running));
        assert_eq!(
            task.enqueue_command(AgentCommand::AppendMessage(Message::text("second"))),
            Err(AgentCommandError::CannotAppend {
                state: AgentState::Running,
            })
        );

        let action = task
            .pop_action()
            .expect("the queued transition is valid")
            .expect("one action is queued");
        assert!(matches!(
            action,
            TaskAction::TaskInput {
                message,
                starts_task: true,
            } if message.as_str() == "first"
        ));
        assert!(task.is_running());
    }

    #[test]
    fn mailbox_preserves_order_when_commands_cross_an_idle_boundary() {
        let mut task = TaskState::new();
        task.enqueue_command(AgentCommand::AppendMessage(Message::text("first")))
            .expect("append is valid");
        task.enqueue_command(AgentCommand::Cancel)
            .expect("cancel validates against the queued append");
        task.enqueue_command(AgentCommand::AppendMessage(Message::text("replacement")))
            .expect("append validates against the queued cancel");

        assert_eq!(projected_state(&task), Ok(AgentState::Running));
        assert!(matches!(
            task.pop_action().expect("valid queue"),
            Some(TaskAction::TaskInput {
                starts_task: true,
                ..
            })
        ));
        assert!(matches!(
            task.pop_action().expect("valid queue"),
            Some(TaskAction::Cancel)
        ));
        assert!(matches!(task.phase, TaskPhase::Idle));
        assert!(matches!(
            task.pop_action().expect("valid queue"),
            Some(TaskAction::TaskInput {
                message,
                starts_task: true,
            }) if message.as_str() == "replacement"
        ));
        assert!(task.is_running());
    }

    #[test]
    fn pending_approval_payload_is_owned_and_consumed_by_the_phase() {
        let mut task = TaskState::new();
        task.enqueue_task_input(Message::text("start"));
        let _ = task.pop_action().expect("valid queue");

        task.await_approval(vec!["sig-a".into(), "sig-b".into()])
            .expect("running tasks may await approval");

        task.enqueue_command(AgentCommand::ApprovalResult(ApprovalDecision::Approved))
            .expect("the matching decision is accepted");

        let action = task
            .pop_action()
            .expect("the queued transition is valid")
            .expect("one action is queued");
        assert!(matches!(
            action,
            TaskAction::ApprovalResult {
                grant_signatures,
                ..
            } if grant_signatures == ["sig-a", "sig-b"]
        ));
        assert!(task.is_running());
    }
}
