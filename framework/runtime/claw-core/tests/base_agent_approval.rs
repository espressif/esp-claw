#![allow(clippy::unwrap_used, clippy::expect_used)]

//! Integration tests for `BaseAgent`'s human-in-the-loop approval flow.
//!
//! Approval is now **permission-driven**: a tool call whose [`PermissionPolicy`]
//! returns `Ask` pauses the agent into `AwaitingApproval` (there is no
//! model-callable `request_approval` tool). A human resolves (or cancels) the
//! pending decision; the recorded transcript and the resumed run reflect it, and
//! a recorded grant lets the retried call run without asking again.

mod common;

use std::sync::Arc;

use claw_core::agent::{
    AgentCommandError, AgentId, AgentState, ApprovalDecision, ApprovalId, CancelReason, TickOutcome,
};
use claw_permission::{AskAtOrAbove, PermissionPolicy, RiskClass};
use claw_tool::{Tool, ToolGroup, ToolSet};
use common::{
    agent_builder, block_on, body_echo_call, body_echo_call_id, body_plain_text, builder_with_view,
    capturing_llm, scripted_llm, transcript_contents, EchoTool, TestAgent, TestAgentBuilder,
    TestLlm,
};

/// A policy that asks for approval on every tool call (every action is at least
/// `Safe` risk), so a plain `echo` call exercises the approval path.
fn ask_everything() -> Arc<dyn PermissionPolicy> {
    Arc::new(AskAtOrAbove::new(RiskClass::Safe))
}

/// The single caller tool (`echo`) used to trigger the policy.
fn echo_tools() -> ToolSet {
    ToolSet::from_groups([ToolGroup::new("echo", [Tool::new(EchoTool)])]).expect("tools")
}

/// A `BaseAgentBuilder` wired with the echo tool, the ask-everything policy, and
/// an identity — ready to `.build()`.
fn asking_builder<H: claw_interface::http::ClawHttp>(
    llm: TestLlm<H>,
    dir: impl AsRef<str>,
) -> TestAgentBuilder<H> {
    agent_builder(llm, AgentId(1), dir)
        .with_tools(echo_tools())
        .with_permission_policy(ask_everything())
        .with_identity(1, "worker")
}

/// Build an asking agent over fresh disk memory with the given scripted LLM.
fn build_agent<H: claw_interface::http::ClawHttp>(name: &str, llm: TestLlm<H>) -> TestAgent<H> {
    let dir = common::test_output_dir(name);
    asking_builder(llm, dir.display().to_string())
        .build()
        .expect("build")
}

// ---------------------------------------------------------------------------

#[test]
fn risky_tool_pauses_for_approval() {
    let mut agent = build_agent(
        "appr_request_pauses",
        scripted_llm(vec![body_echo_call("x")]),
    );

    agent.run("do it");
    assert!(matches!(
        block_on(agent.tick()),
        TickOutcome::AwaitingApproval { ref summary, .. } if summary.contains("echo")
    ));
    assert!(!agent.is_running());
}

#[test]
fn approve_resumes_and_records_decision() {
    let dir = common::test_output_dir("appr_approve_records");
    let (builder, view) = builder_with_view(
        scripted_llm(vec![body_echo_call("x"), body_plain_text("done")]),
        AgentId(1),
        dir.display().to_string(),
    );
    let mut agent = builder
        .with_tools(echo_tools())
        .with_permission_policy(ask_everything())
        .with_identity(1, "worker")
        .build()
        .expect("build");

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };

    agent
        .resolve_approval(id, ApprovalDecision::Approved)
        .expect("resolve accepted");

    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "done"));
    let transcript = transcript_contents(&view);
    assert!(transcript
        .iter()
        .any(|c| c.contains("approved by the human")));
}

#[test]
fn reject_resumes_and_records_reason() {
    let dir = common::test_output_dir("appr_reject_records");
    let (builder, view) = builder_with_view(
        scripted_llm(vec![body_echo_call("x"), body_plain_text("ok")]),
        AgentId(1),
        dir.display().to_string(),
    );
    let mut agent = builder
        .with_tools(echo_tools())
        .with_permission_policy(ask_everything())
        .with_identity(1, "worker")
        .build()
        .expect("build");

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };

    agent
        .resolve_approval(id, ApprovalDecision::Rejected("too risky".into()))
        .expect("resolve accepted");

    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "ok"));
    let transcript = transcript_contents(&view);
    assert!(transcript
        .iter()
        .any(|c| c.contains("rejected by the human") && c.contains("too risky")));
}

/// After approval, the recorded grant lets the *retried* call actually run
/// (instead of asking again).
#[test]
fn grant_lets_retried_call_run() {
    let dir = common::test_output_dir("appr_grant_retried");
    let (builder, view) = builder_with_view(
        scripted_llm(vec![
            body_echo_call_id("t1", "first"),  // asked
            body_echo_call_id("t2", "second"), // retried: now granted -> runs
            body_plain_text("done"),
        ]),
        AgentId(1),
        dir.display().to_string(),
    );
    let mut agent = builder
        .with_tools(echo_tools())
        .with_permission_policy(ask_everything())
        .with_identity(1, "worker")
        .build()
        .expect("build");

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };
    agent
        .resolve_approval(id, ApprovalDecision::Approved)
        .expect("resolve accepted");

    assert_eq!(common::run_to_completion(&mut agent), "done");

    // The second (granted) echo actually executed.
    let transcript = transcript_contents(&view);
    assert!(
        transcript
            .iter()
            .any(|c| c.contains("echo:") && c.contains("second")),
        "granted retry did not run: {transcript:?}"
    );
}

#[test]
fn wrong_approval_id_is_rejected_and_stays_awaiting() {
    let mut agent = build_agent(
        "appr_wrong_id",
        scripted_llm(vec![body_echo_call("x"), body_plain_text("after")]),
    );

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };

    assert_eq!(
        agent.resolve_approval(ApprovalId(999), ApprovalDecision::Approved),
        Err(AgentCommandError::ApprovalMismatch {
            expected: id,
            got: ApprovalId(999),
        })
    );

    // Still awaiting: no iteration runs, no scripted body is consumed.
    assert!(matches!(block_on(agent.tick()), TickOutcome::Idle));

    agent
        .resolve_approval(id, ApprovalDecision::Approved)
        .expect("resolve accepted");
    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "after"));
}

#[test]
fn approve_twice_is_rejected() {
    let mut agent = build_agent(
        "appr_approve_twice",
        scripted_llm(vec![body_echo_call("x")]),
    );

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };

    agent
        .resolve_approval(id, ApprovalDecision::Approved)
        .expect("first resolve accepted");

    // Projected state is now Running, so a second resolve is illegal.
    assert_eq!(
        agent.resolve_approval(id, ApprovalDecision::Approved),
        Err(AgentCommandError::NotAwaitingApproval {
            state: AgentState::Running
        })
    );
}

#[test]
fn resolve_when_idle_is_rejected() {
    let mut agent = build_agent("appr_resolve_idle", scripted_llm(vec![]));

    assert_eq!(
        agent.resolve_approval(ApprovalId(0), ApprovalDecision::Approved),
        Err(AgentCommandError::NotAwaitingApproval {
            state: AgentState::Idle
        })
    );
}

#[test]
fn cancel_while_awaiting_clears_pending_and_records_marker() {
    let dir = common::test_output_dir("appr_cancel_awaiting");
    let (builder, view) = builder_with_view(
        scripted_llm(vec![body_echo_call("x")]),
        AgentId(1),
        dir.display().to_string(),
    );
    let mut agent = builder
        .with_tools(echo_tools())
        .with_permission_policy(ask_everything())
        .with_identity(1, "worker")
        .build()
        .expect("build");

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };

    agent
        .cancel(CancelReason::UserRequested)
        .expect("cancel accepted");

    assert!(matches!(
        block_on(agent.tick()),
        TickOutcome::Cancelled {
            reason: CancelReason::UserRequested
        }
    ));

    let transcript = transcript_contents(&view);
    assert!(transcript.iter().any(|c| c.contains("interrupted")));

    // Pending approval cleared; the agent is Idle again.
    assert_eq!(
        agent.resolve_approval(id, ApprovalDecision::Approved),
        Err(AgentCommandError::NotAwaitingApproval {
            state: AgentState::Idle
        })
    );
}

#[test]
fn append_while_awaiting_is_included_after_approval() {
    let dir = common::test_output_dir("appr_append_awaiting");
    let (llm, http) = capturing_llm(vec![body_echo_call("x"), body_plain_text("final")]);
    let mut agent = asking_builder(llm, dir.display().to_string())
        .build()
        .expect("build");

    agent.run("go");
    let id = match block_on(agent.tick()) {
        TickOutcome::AwaitingApproval { id, .. } => id,
        other => panic!("expected AwaitingApproval, got {other:?}"),
    };

    agent.append_message("more info");
    agent
        .resolve_approval(id, ApprovalDecision::Approved)
        .expect("resolve accepted");

    assert!(matches!(block_on(agent.tick()), TickOutcome::Yielded { text } if text == "final"));

    assert_eq!(http.call_count(), 2);
    let second_body = http.captured_bodies()[1].to_string();
    assert!(second_body.contains("more info"));
    assert!(second_body.contains("approved by the human"));
}
