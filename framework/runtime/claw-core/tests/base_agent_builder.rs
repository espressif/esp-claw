#![allow(clippy::unwrap_used, clippy::expect_used)]

//! Integration tests for `BaseAgent`'s builder: tool merging with the built-ins,
//! the tool/LLM-support contract, and how the system prompt reaches the LLM.

mod common;

use claw_core::agent::{AgentId, BaseAgentBuildError, TickOutcome};
use claw_core::{
    Tool, ToolGroup, ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput, ToolSet,
};
use common::{
    agent_builder, body_echo_call, body_end_conversation, body_plain_text, capturing_llm,
    run_to_completion, scripted_llm, scripted_llm_no_tools, test_output_dir,
};

/// The caller-provided `echo` tool set used by several builder tests.
fn echo_tools() -> ToolSet {
    ToolSet::from_groups([ToolGroup::new("echo_group", [Tool::new(common::EchoTool)])])
        .expect("tools")
}

#[test]
fn build_without_tools_runs_plain_text() {
    let dir = test_output_dir("build_without_tools_runs_plain_text");
    let mut agent = agent_builder(
        scripted_llm(vec![body_plain_text("pong")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("ping");
    assert_eq!(run_to_completion(&mut agent), "pong");
}

#[test]
fn build_with_tools_runs_a_tool_round() {
    let dir = test_output_dir("build_with_tools_runs_a_tool_round");
    let mut agent = agent_builder(
        scripted_llm(vec![body_echo_call("hi"), body_plain_text("done")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .with_tools(echo_tools())
    .build()
    .expect("build");

    agent.run("use the echo tool");
    // First round issues the tool call (still working); the tool result feeds the
    // next LLM round, which yields the final text.
    assert!(matches!(agent.tick(), TickOutcome::Working));
    assert!(matches!(agent.tick(), TickOutcome::Yielded { text } if text == "done"));
}

#[test]
fn built_in_end_conversation_is_available_with_tools() {
    let dir = test_output_dir("built_in_end_conversation_is_available_with_tools");
    let mut agent = agent_builder(
        scripted_llm(vec![body_end_conversation("bye")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .with_tools(echo_tools())
    .build()
    .expect("build");

    agent.run("wrap up");
    assert!(matches!(
        agent.tick(),
        TickOutcome::Ended { final_message } if final_message == "bye"
    ));
}

#[test]
fn tools_with_unsupported_llm_is_error() {
    let dir = test_output_dir("tools_with_unsupported_llm_is_error");
    let result = agent_builder(
        scripted_llm_no_tools(vec![]),
        AgentId(1),
        dir.display().to_string(),
    )
    .with_tools(echo_tools())
    .build();

    assert!(matches!(result, Err(BaseAgentBuildError::ToolsUnsupported)));
}

#[test]
fn no_tools_with_unsupported_llm_builds_and_runs() {
    let dir = test_output_dir("no_tools_with_unsupported_llm_builds_and_runs");
    let mut agent = agent_builder(
        scripted_llm_no_tools(vec![body_plain_text("hi")]),
        AgentId(1),
        dir.display().to_string(),
    )
    .build()
    .expect("build");

    agent.run("say hi");
    assert_eq!(run_to_completion(&mut agent), "hi");
}

/// A caller tool whose name collides with the built-in `end_conversation`.
struct ClashTool;

impl ToolHandler for ClashTool {
    fn name(&self) -> &str {
        "end_conversation"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"end_conversation"}}"#
    }

    fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        Ok(ToolOutput {
            output: String::new(),
            ok: true,
        })
    }
}

#[test]
fn caller_tool_name_clashing_with_builtin_is_error() {
    let dir = test_output_dir("caller_tool_name_clashing_with_builtin_is_error");
    let clashing =
        ToolSet::from_groups([ToolGroup::new("clash", [Tool::new(ClashTool)])]).expect("tools");
    let result = agent_builder(scripted_llm(vec![]), AgentId(1), dir.display().to_string())
        .with_tools(clashing)
        .build();

    assert!(matches!(result, Err(BaseAgentBuildError::Tools(_))));
}

#[test]
fn system_prompt_is_sent_to_the_llm() {
    let dir = test_output_dir("system_prompt_is_sent_to_the_llm");
    let (llm, http) = capturing_llm(vec![body_plain_text("pong")]);
    let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
        .with_system_prompt("You are PROMPTY.")
        .build()
        .expect("build");

    agent.run("ping");
    assert_eq!(run_to_completion(&mut agent), "pong");

    let bodies = http.captured_bodies();
    let msgs = bodies[0]["messages"].as_array().expect("messages");
    assert!(msgs
        .iter()
        .any(|m| m["role"] == "system" && m["content"].as_str().unwrap_or("").contains("PROMPTY")));
}

#[test]
fn default_empty_system_prompt_sends_no_system_message() {
    let dir = test_output_dir("default_empty_system_prompt_sends_no_system_message");
    let (llm, http) = capturing_llm(vec![body_plain_text("pong")]);
    let mut agent = agent_builder(llm, AgentId(1), dir.display().to_string())
        .build()
        .expect("build");

    agent.run("ping");
    assert_eq!(run_to_completion(&mut agent), "pong");

    let bodies = http.captured_bodies();
    let msgs = bodies[0]["messages"].as_array().expect("messages");
    assert!(!msgs.iter().any(|m| m["role"] == "system"));
}
