#![allow(clippy::unwrap_used, clippy::expect_used)]

//! Pause / resume FSM tests for `BaseAgent`.
//!
//! These prove the pause-state contract end to end: a paused agent runs no LLM
//! iteration (proved by strict empty/exact scripts that panic on an extra call),
//! `resume` rewinds it back to `Running`, messages appended while paused join the
//! current task, and the pause/resume commands are rejected outside their legal
//! state.

mod common;

use claw_core::agent::{AgentCommandError, AgentId, AgentState, CancelReason, TickOutcome};
use claw_tool::{Tool, ToolGroup, ToolSet};
use common::{
    agent_builder, block_on, body_echo_call, body_plain_text, capturing_llm, scripted_llm,
    TestAgent, TestLlm,
};

/// A `ToolSet` exposing only the `echo` test tool.
fn echo_tools() -> ToolSet {
    ToolSet::from_groups([ToolGroup::new("echo_group", [Tool::new(common::EchoTool)])])
        .expect("tools")
}

/// Build an agent over the given LLM with a unique on-disk transcript dir.
fn build_agent<H: claw_interface::http::ClawHttp>(name: &str, llm: TestLlm<H>) -> TestAgent<H> {
    let dir = common::test_output_dir(name);
    agent_builder(llm, AgentId(1), dir.display().to_string())
        .build()
        .expect("build")
}

#[test]
fn pause_before_first_tick_prevents_iteration() {
    // Empty script: any LLM iteration would panic, proving none runs while paused.
    let mut agent = build_agent("pause_before_first_tick", scripted_llm(vec![]));

    agent.run("work");
    agent
        .pause()
        .expect("pause accepted while Running (projected)");

    assert!(!agent.is_running());
    assert!(matches!(block_on(agent.tick()), TickOutcome::Idle));
}

#[test]
fn resume_runs_the_pending_task() {
    let mut agent = build_agent(
        "pause_resume_runs_pending",
        scripted_llm(vec![body_plain_text("pong")]),
    );

    agent.run("work");
    agent.pause().expect("pause accepted");
    assert!(matches!(block_on(agent.tick()), TickOutcome::Idle));

    agent.resume().expect("resume accepted");
    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "pong"));
    assert!(!agent.is_running());
}

#[test]
fn pause_midway_through_a_multi_iteration_task() {
    // Strict script => exactly two LLM calls: the tool round, then the answer.
    let mut agent = {
        let dir = common::test_output_dir("pause_midway_multi_iteration");
        agent_builder(
            scripted_llm(vec![body_echo_call("hi"), body_plain_text("done")]),
            AgentId(1),
            dir.display().to_string(),
        )
        .with_tools(echo_tools())
        .build()
        .expect("build")
    };

    agent.run("work");
    // First iteration is a tool round: stays Running and reports Working.
    assert!(matches!(block_on(agent.tick()), TickOutcome::Working));
    assert!(agent.is_running());

    agent.pause().expect("pause accepted");
    // No second LLM call is consumed while paused.
    assert!(matches!(block_on(agent.tick()), TickOutcome::Idle));

    agent.resume().expect("resume accepted");
    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "done"));
}

#[test]
fn append_while_paused_is_included_after_resume() {
    let (llm, http) = capturing_llm(vec![body_plain_text("answer")]);
    let mut agent = build_agent("pause_append_while_paused", llm);

    agent.run("first goal");
    agent.pause().expect("pause accepted");
    assert!(matches!(block_on(agent.tick()), TickOutcome::Idle));

    // The appended message is queued and joins the current task on resume.
    agent.append_message("extra context");
    agent.resume().expect("resume accepted");
    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "answer"));

    // Exactly one LLM call, carrying both the original goal and the appended text.
    assert_eq!(http.call_count(), 1);
    let bodies = http.captured_bodies();
    let first = bodies[0].to_string();
    assert!(first.contains("first goal"));
    assert!(first.contains("extra context"));
}

#[test]
fn double_pause_is_rejected() {
    let mut agent = build_agent("pause_double_pause", scripted_llm(vec![]));

    agent.run("work");
    agent.pause().expect("first pause accepted");
    assert_eq!(
        agent.pause(),
        Err(AgentCommandError::CannotPause {
            state: AgentState::Paused
        })
    );
}

#[test]
fn double_resume_is_rejected() {
    let mut agent = build_agent("pause_double_resume", scripted_llm(vec![]));

    agent.run("work");
    agent.pause().expect("pause accepted");
    agent.resume().expect("first resume accepted");
    assert_eq!(
        agent.resume(),
        Err(AgentCommandError::CannotResume {
            state: AgentState::Running
        })
    );
}

#[test]
fn is_running_is_false_while_paused() {
    let mut agent = build_agent("pause_is_running_false", scripted_llm(vec![]));

    agent.run("work");
    agent.pause().expect("pause accepted");
    assert!(!agent.is_running());
}

#[test]
fn cancel_while_paused_reports_cancelled() {
    let mut agent = build_agent("pause_cancel_while_paused", scripted_llm(vec![]));

    agent.run("work");
    agent.pause().expect("pause accepted");
    agent
        .cancel(CancelReason::UserRequested)
        .expect("cancel accepted while paused");

    assert!(matches!(
        block_on(agent.tick()),
        TickOutcome::Cancelled {
            reason: CancelReason::UserRequested
        }
    ));
    assert!(!agent.is_running());
}
