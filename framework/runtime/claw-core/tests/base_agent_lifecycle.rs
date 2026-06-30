#![allow(clippy::unwrap_used, clippy::expect_used)]

//! Integration tests for `BaseAgent`'s task lifecycle: a non-terminal yield, the
//! idle state that follows it, re-tasking after every terminal outcome (ended,
//! cancelled, failed), memory continuity across a yield, and the abort flag that
//! preempts a tick before the LLM call.

mod common;

use claw_core::agent::{AgentId, CancelReason, RetryPolicy, TickOutcome};
use common::{
    agent_builder, body_end_conversation, body_plain_text, capturing_llm, run_to_completion,
    scripted_llm, scripted_llm_steps,
};

/// A plain-text answer is non-terminal: the agent yields, goes idle, and the next
/// appended message continues the SAME conversation (the first turn is in context).
#[test]
fn yield_is_non_terminal_then_continues() {
    let dir = common::test_output_dir("life_yield_continues");
    let (llm, http) = capturing_llm(vec![body_plain_text("a1"), body_plain_text("a2")]);
    let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    agent.run("first");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "a1"));
    assert!(!agent.is_running());

    agent.append_message("second");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "a2"));

    assert_eq!(http.call_count(), 2);
    let second_body = http.captured_bodies()[1].to_string();
    assert!(second_body.contains("first"));
}

/// After a yield, with no new input, further ticks are `Idle` and make NO LLM
/// call (the strict single-step script would panic on an extra round).
#[test]
fn idle_after_yield_makes_no_llm_call() {
    let dir = common::test_output_dir("life_idle_after_yield");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("x")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("go");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "x"));
    assert!(matches!(agent.tick(), TickOutcome::Idle));
    assert!(matches!(agent.tick(), TickOutcome::Idle));
}

/// `end_conversation` is terminal; after it the agent is idle and a new `run`
/// starts a fresh task.
#[test]
fn retask_after_ended() {
    let dir = common::test_output_dir("life_retask_after_ended");
    let mut agent = agent_builder(
        scripted_llm(vec![
            body_end_conversation("bye"),
            body_plain_text("hello again"),
        ]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("a");
    let out = agent.tick();
    assert!(matches!(out, TickOutcome::Ended { ref final_message } if final_message == "bye"));
    assert!(out.is_terminal());

    agent.run("b");
    assert_eq!(run_to_completion(&mut agent), "hello again");
}

/// `cancel` is terminal on the next tick; the agent is then reusable.
#[test]
fn retask_after_cancelled() {
    let dir = common::test_output_dir("life_retask_after_cancelled");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("answer")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("a");
    agent
        .cancel(CancelReason::UserRequested)
        .expect("cancel accepted");
    let out = agent.tick();
    assert!(matches!(
        out,
        TickOutcome::Cancelled {
            reason: CancelReason::UserRequested
        }
    ));
    assert!(out.is_terminal());

    agent.run("b");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "answer"));
}

/// A failed LLM round is terminal; the agent goes idle and a new `run` recovers.
///
/// The agent runs with `RetryPolicy::none`, so a single transport error is not
/// retried and surfaces directly as `Failed`; the next `Ok` step serves the
/// recovery task. (With the default policy a transient error would be retried
/// and would silently consume the recovery response.)
#[test]
fn retask_after_failed() {
    let dir = common::test_output_dir("life_retask_after_failed");
    let mut agent = agent_builder(
        scripted_llm_steps(vec![Err("boom".into()), Ok(body_plain_text("recovered"))]),
        AgentId(1),
        dir.display().to_string(),
    )
    .with_retry_policy(RetryPolicy::none())
    .build()
    .expect("build");

    agent.run("a");
    let out = agent.tick();
    assert!(matches!(out, TickOutcome::Failed(_)));
    assert!(out.is_terminal());
    assert!(!agent.is_running());

    agent.run("b");
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "recovered"));
}

/// `is_terminal` classifies the terminal outcomes from the non-terminal ones.
#[test]
fn is_terminal_classification() {
    assert!(TickOutcome::Ended {
        final_message: "x".into()
    }
    .is_terminal());
    assert!(TickOutcome::Cancelled {
        reason: CancelReason::Shutdown
    }
    .is_terminal());
    assert!(!TickOutcome::Working.is_terminal());
    assert!(!TickOutcome::Idle.is_terminal());
    assert!(!TickOutcome::Yielded { text: "y".into() }.is_terminal());
}

/// Aborting before an iteration preempts the next tick before the LLM call: the
/// tick returns `Working` and stays running, then the following tick reaches the
/// LLM (the flag is consumed).
#[test]
fn abort_before_iteration_reruns_next_tick() {
    let dir = common::test_output_dir("life_abort_reruns");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("hi");
    agent.abort_handle().abort();
    assert!(matches!(agent.tick(), TickOutcome::Working));
    assert!(agent.is_running());
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "pong"));
}

/// A cloned `AgentAbortHandle` shares the same flag, so aborting via the clone
/// preempts the next tick exactly like the original.
#[test]
fn cloned_abort_handle_shares_flag() {
    let dir = common::test_output_dir("life_cloned_abort");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("hi");
    let handle = agent.abort_handle();
    let cloned = handle.clone();
    cloned.abort();
    assert!(matches!(agent.tick(), TickOutcome::Working));
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "pong"));
}
