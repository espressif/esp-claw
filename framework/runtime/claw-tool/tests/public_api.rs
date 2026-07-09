use core::future::Future;
use core::task::{Context, Poll};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::Arc;
use std::task::Wake;

use anyhow::{anyhow, Result};
use claw_checkpoint::{DurablePart, DurablePartError};
use claw_tool::{
    RawToolInvocation, RetryCount, SyncToolHandler, Tool, ToolError, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolRegistry, ToolRegistryError, ToolResult, ToolRunOutcome,
    ToolRunner, ToolSetHandle, ToolSpec,
};

#[test]
fn local_tool_runs_through_public_tool_surface() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    tool_set.add_tool(Tool::from_sync(EchoTool))?;

    let handle = tool_set.begin()?;
    assert_eq!(
        handle.schemas_json(),
        r#"[{"type":"function","function":{"name":"echo"}}]"#
    );
    assert_eq!(handle.tool_context(), "Echoes the normalized arguments.");

    let call = invocation("echo", r#" { "message": "hi" } "#)?;
    let outcome = run_with_default_gate(&handle, &call)?;

    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: r#"{ "message": "hi" }"#.into(),
            ok: true,
        }
    );
    Ok(())
}

#[test]
fn temporary_disable_blocks_runner_but_keeps_tool_context() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    tool_set.add_tool(Tool::from_sync(EchoTool))?;

    tool_set.temporarily_disable_tool("echo".into())?;

    {
        let handle = tool_set.begin()?;
        assert_eq!(
            handle.schemas_json(),
            r#"[{"type":"function","function":{"name":"echo"}}]"#
        );
        assert_eq!(
            handle.extra_tool_context(),
            "Tool `echo` is temporarily unavailable."
        );

        let call = invocation("echo", "{}")?;
        let outcome = run_with_default_gate(&handle, &call)?;
        assert_eq!(
            outcome,
            ToolRunOutcome::Blocked {
                content: "tool is temporarily unavailable: echo".into(),
            }
        );
    }

    tool_set.clear_temporary_tools();

    let handle = tool_set.begin()?;
    assert_eq!(handle.extra_tool_context(), "no extra tool context");

    let call = invocation("echo", "{}")?;
    let outcome = run_with_default_gate(&handle, &call)?;
    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: "{}".into(),
            ok: true,
        }
    );
    Ok(())
}

#[test]
fn registry_tools_appear_only_after_registry_is_started() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    registry.register(Tool::from_sync(EchoTool))?;
    let mut tool_set = registry.tool_set();

    {
        let handle = tool_set.begin()?;
        assert_eq!(handle.schemas_json(), "no schemas");

        let call = invocation("echo", "{}")?;
        let outcome = run_with_default_gate(&handle, &call)?;
        assert_eq!(
            outcome,
            ToolRunOutcome::Ran {
                content: "tool not found: echo".into(),
                ok: false,
            }
        );
    }

    registry.start_all()?;

    let handle = tool_set.begin()?;
    assert_eq!(
        handle.schemas_json(),
        r#"[{"type":"function","function":{"name":"echo"}}]"#
    );

    let call = invocation("echo", "{}")?;
    let outcome = run_with_default_gate(&handle, &call)?;
    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: "{}".into(),
            ok: true,
        }
    );
    Ok(())
}

#[test]
fn tool_set_durable_state_tracks_tool_metadata() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    assert_eq!(tool_set.generation(), 0);

    tool_set.add_tool(Tool::from_sync(EchoTool))?;
    assert_eq!(tool_set.generation(), 1);

    tool_set.temporarily_disable_tool("echo".into())?;
    assert_eq!(tool_set.generation(), 2);

    tool_set.temporarily_disable_tool("echo".into())?;
    assert_eq!(tool_set.generation(), 2);

    let blob = tool_set.export_state()?;
    let state: serde_json::Value = serde_json::from_slice(&blob.bytes)?;
    assert_eq!(state["registry_version"], 0);
    assert_eq!(state["tools"]["echo"]["source"], "local");
    assert_eq!(state["tools"]["echo"]["state"], "temporarily_disabled");
    assert!(state["tools"]["echo"].get("schema").is_none());
    Ok(())
}

#[test]
fn restored_tool_set_does_not_dirty_generation() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    tool_set.add_tool(Tool::from_sync(EchoTool))?;
    tool_set.temporarily_disable_tool("echo".into())?;
    let blob = tool_set.export_state()?;

    let mut restored = registry.tool_set();
    restored.add_tool(Tool::from_sync(EchoTool))?;
    restored.restore_state(blob.as_slice())?;
    assert_eq!(restored.generation(), 0);

    let handle = restored.begin()?;
    let outcome = run_with_default_gate(&handle, &invocation("echo", "{}")?)?;
    assert_eq!(
        outcome,
        ToolRunOutcome::Blocked {
            content: "tool is temporarily unavailable: echo".into(),
        }
    );
    Ok(())
}

#[test]
fn restored_registry_reuses_tool_state_without_dirtying_generation() -> Result<()> {
    let registry = ToolRegistry::new();
    registry.register(Tool::from_sync(EchoTool))?;
    registry.disable("echo")?;

    let blob = registry.export_state()?;
    let restored = <ToolRegistry as DurablePart>::restore_from_state(blob.as_slice())?;
    assert_eq!(restored.generation(), 0);

    restored.register(Tool::from_sync(EchoTool))?;
    assert_eq!(restored.generation(), 0);

    restored.enable("echo")?;
    assert_eq!(restored.generation(), 1);
    Ok(())
}

#[test]
fn registry_rolls_back_mutation_when_checkpoint_hook_fails() -> Result<()> {
    let registry = ToolRegistry::new();
    registry.set_checkpoint_hook(|_, _| {
        Err(DurablePartError::InvalidState("checkpoint failed").into())
    });

    assert!(matches!(
        registry.register(Tool::from_sync(EchoTool)),
        Err(ToolRegistryError::Checkpoint(_))
    ));
    let blob = registry.export_state()?;
    let state: serde_json::Value = serde_json::from_slice(&blob.bytes)?;
    assert!(state["tools"]
        .as_object()
        .filter(|tools| tools.is_empty())
        .is_some());

    assert!(matches!(
        registry.register(Tool::from_sync(EchoTool)),
        Err(ToolRegistryError::Checkpoint(_))
    ));
    Ok(())
}

#[test]
fn invocation_normalizes_empty_arguments() {
    let call = ToolInvocation::try_from(RawToolInvocation {
        id: None,
        name: "demo",
        arguments_json: "  ",
    });

    assert!(matches!(call, Ok(call) if call.arguments_json() == "{}"));
}

#[test]
fn invocation_rejects_non_object_arguments() {
    let call = ToolInvocation::try_from(RawToolInvocation {
        id: None,
        name: "demo",
        arguments_json: "[]",
    });

    assert!(matches!(
        call,
        Err(error) if matches!(error.error, ToolError::InvalidArgumentsJson(_))
    ));
}

#[test]
fn runner_retries_tool_according_to_retry_count() -> Result<()> {
    let attempts = Arc::new(AtomicU32::new(0));
    let outcome = run_retry_tool(RetryCount::extra(1), Arc::clone(&attempts))?;

    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: "ok".into(),
            ok: true,
        }
    );
    assert_eq!(attempts.load(Ordering::SeqCst), 2);
    Ok(())
}

#[test]
fn runner_does_not_retry_by_default() -> Result<()> {
    let attempts = Arc::new(AtomicU32::new(0));
    let outcome = run_retry_tool(RetryCount::none(), Arc::clone(&attempts))?;

    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: "tool invocation rejected: try again".into(),
            ok: false,
        }
    );
    assert_eq!(attempts.load(Ordering::SeqCst), 1);
    Ok(())
}

struct EchoTool;

impl ToolSpec for EchoTool {
    fn name(&self) -> &str {
        "echo"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"echo"}}"#
    }

    fn usage(&self) -> Option<&str> {
        Some("Echoes the normalized arguments.")
    }
}

impl SyncToolHandler for EchoTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        if call.name() != self.name() {
            return Err(ToolError::NotFound(call.name().to_owned()).into());
        }
        Ok(ToolOutput {
            output: call.arguments_json().to_owned(),
            ok: true,
        })
    }
}

struct FailBeforeSuccess {
    attempts: Arc<AtomicU32>,
    retry_count: RetryCount,
}

impl ToolSpec for FailBeforeSuccess {
    fn name(&self) -> &str {
        "retry_demo"
    }

    fn schema(&self) -> &str {
        "{}"
    }

    fn retry_count(&self) -> RetryCount {
        self.retry_count
    }
}

impl SyncToolHandler for FailBeforeSuccess {
    fn invoke(&self, _call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        if self.attempts.fetch_add(1, Ordering::SeqCst) == 0 {
            return Err(ToolInvokeError::new(ToolError::InvokeRejected(
                "try again".into(),
            )));
        }
        Ok(ToolOutput {
            output: "ok".into(),
            ok: true,
        })
    }
}

struct NoopWake;

impl Wake for NoopWake {
    fn wake(self: Arc<Self>) {}
}

fn invocation(name: &'static str, arguments_json: &'static str) -> Result<ToolInvocation<'static>> {
    ToolInvocation::try_from(RawToolInvocation {
        id: None,
        name,
        arguments_json,
    })
    .map_err(|error| anyhow!("{error:?}"))
}

fn run_with_default_gate(
    handle: &ToolSetHandle<'_>,
    call: &ToolInvocation<'_>,
) -> Result<ToolRunOutcome> {
    let runner = ToolRunner::new(handle, None);
    poll_ready(runner.run(call))
}

fn run_retry_tool(retry_count: RetryCount, attempts: Arc<AtomicU32>) -> Result<ToolRunOutcome> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    tool_set.add_tool(Tool::from_sync(FailBeforeSuccess {
        attempts,
        retry_count,
    }))?;
    let handle = tool_set.begin()?;
    let call = invocation("retry_demo", "{}")?;
    run_with_default_gate(&handle, &call)
}

fn poll_ready<T>(future: impl Future<Output = T>) -> Result<T> {
    let waker = std::task::Waker::from(Arc::new(NoopWake));
    let mut context = Context::from_waker(&waker);
    let mut future = std::pin::pin!(future);
    match future.as_mut().poll(&mut context) {
        Poll::Ready(output) => Ok(output),
        Poll::Pending => Err(anyhow!("future was pending")),
    }
}
