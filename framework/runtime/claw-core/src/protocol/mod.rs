//! Inter-agent protocol: shared task state, commands, and events.

mod command;
mod event;
mod ids;
mod queues;
mod task;

pub use crate::iteration_loop::IterationId;
pub use command::Command;
pub use event::AgentEvent;
pub use ids::{IdParseError, StepId, TaskId, WorkerId};
pub use queues::{AgentEventQueue, CommandQueue};
pub use task::{ApprovalRecord, TaskContract, TaskStatus};
