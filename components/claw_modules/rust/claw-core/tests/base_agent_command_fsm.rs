//! Command-validation / FSM tests for `BaseAgent`'s public command surface.
//!
//! These exercise the `Result`-returning commands at the integration level:
//! illegal commands are rejected with the right [`AgentCommandError`], the agent
//! is left unchanged, and validation is against the *projected* state so a batch
//! of commands queued between ticks is checked in order.

#![allow(clippy::unwrap_used, clippy::expect_used)]

mod common;

use claw_core::agent::{
    AgentCommand, AgentCommandError, AgentId, AgentState, ApprovalDecision, ApprovalId,
    CancelReason, TickOutcome,
};
use common::{agent_builder, body_plain_text, scripted_llm, TestAgent};

fn idle_agent(name: &str) -> TestAgent<claw_interface::ScriptedHttp> {
    let dir = common::test_output_dir(name);
    agent_builder(scripted_llm(vec![]), AgentId(1), dir.display().to_string())
        .build()
        .expect("build")
}

// -- Rejections from Idle ---------------------------------------------------

#[test]
fn pause_from_idle_is_rejected() {
    let mut agent = idle_agent("fsm_pause_from_idle");
    assert_eq!(
        agent.pause(),
        Err(AgentCommandError::CannotPause {
            state: AgentState::Idle
        })
    );
    // Unchanged: still idle, no LLM call (empty script would panic if called).
    assert!(!agent.is_running());
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

#[test]
fn resume_from_idle_is_rejected() {
    let mut agent = idle_agent("fsm_resume_from_idle");
    assert_eq!(
        agent.resume(),
        Err(AgentCommandError::CannotResume {
            state: AgentState::Idle
        })
    );
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

#[test]
fn cancel_from_idle_is_rejected() {
    let mut agent = idle_agent("fsm_cancel_from_idle");
    assert_eq!(
        agent.cancel(CancelReason::UserRequested),
        Err(AgentCommandError::NothingToCancel)
    );
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

#[test]
fn resolve_approval_from_idle_is_rejected() {
    let mut agent = idle_agent("fsm_resolve_from_idle");
    assert_eq!(
        agent.resolve_approval(ApprovalId(0), ApprovalDecision::Approved),
        Err(AgentCommandError::NotAwaitingApproval {
            state: AgentState::Idle
        })
    );
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

// -- Projected-state rejections (no tick between commands) -------------------

#[test]
fn resume_while_running_is_rejected() {
    let dir = common::test_output_dir("fsm_resume_while_running");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    // `run` projects the state to Running; resume is illegal there.
    agent.run("do work");
    assert_eq!(
        agent.resume(),
        Err(AgentCommandError::CannotResume {
            state: AgentState::Running
        })
    );
}

#[test]
fn cancel_then_resume_is_rejected_before_any_tick() {
    let dir = common::test_output_dir("fsm_cancel_then_resume");
    let mut agent = agent_builder(scripted_llm(vec![]), AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    agent.run("do work");
    agent
        .cancel(CancelReason::UserRequested)
        .expect("cancel accepted while a task is queued");
    // Cancel projected the agent back to Idle, so a following resume is rejected
    // instead of being silently dropped.
    assert_eq!(
        agent.resume(),
        Err(AgentCommandError::CannotResume {
            state: AgentState::Idle
        })
    );
}

#[test]
fn double_pause_is_rejected_against_projection() {
    let dir = common::test_output_dir("fsm_double_pause");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("do work"); // projected: Running
    agent.pause().expect("first pause accepted"); // projected: Paused
    assert_eq!(
        agent.pause(),
        Err(AgentCommandError::CannotPause {
            state: AgentState::Paused
        })
    );
}

#[test]
fn a_batch_validated_in_order_then_runs() {
    let dir = common::test_output_dir("fsm_batch_in_order");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    // Queue a whole batch before ticking; each is validated against the running
    // projection: Idle -(run)-> Running -(pause)-> Paused -(resume)-> Running.
    agent.run("do work");
    agent.pause().expect("pause accepted (projected Running)");
    agent.resume().expect("resume accepted (projected Paused)");

    // The single tick drains the batch (ending Running) and runs one iteration.
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "pong"));
    assert!(!agent.is_running());
}

// -- A rejected command must not be enqueued --------------------------------

#[test]
fn rejected_command_does_not_enqueue() {
    let dir = common::test_output_dir("fsm_rejected_not_enqueued");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    // An illegal command is rejected and leaves no trace on the inbox...
    assert_eq!(
        agent.send_command(AgentCommand::Resume),
        Err(AgentCommandError::CannotResume {
            state: AgentState::Idle
        })
    );
    // ...so a normal task afterwards behaves exactly as if it never happened.
    agent.run("do work");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "pong"));
}

#[test]
fn send_command_is_the_validating_funnel() {
    let mut agent = idle_agent("fsm_send_command_funnel");
    // `send_command` performs the same validation as the typed wrappers.
    assert_eq!(
        agent.send_command(AgentCommand::Pause),
        Err(AgentCommandError::CannotPause {
            state: AgentState::Idle
        })
    );
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}
