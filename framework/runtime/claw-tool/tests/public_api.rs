use core::future::Future;
use core::task::{Context, Poll};
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::task::Waker;

use anyhow::{anyhow, Result};
use claw_checkpoint::{CheckpointError, CheckpointStorageError, DurablePart};
use claw_tool::{
    RawToolInvocation, RetryCount, SyncToolHandler, Tool, ToolError, ToolGroup, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolRegistry, ToolRegistryCheckpointError, ToolRegistryError,
    ToolResult, ToolRunOutcome, ToolRunner, ToolSetHandle, ToolSpec,
};

#[test]
fn local_tool_runs_through_public_tool_surface() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    tool_set.add_group(ToolGroup::new("local", true, [Tool::from_sync(EchoTool)]))?;

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
    tool_set.add_group(ToolGroup::new("local", true, [Tool::from_sync(EchoTool)]))?;

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
    registry.register_group(ToolGroup::new("test", true, [Tool::from_sync(EchoTool)]))?;
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
fn registry_rejects_duplicate_tools_across_groups() -> Result<()> {
    let registry = ToolRegistry::new();

    registry.register_group(ToolGroup::new("first", true, [Tool::from_sync(EchoTool)]))?;
    let err = registry
        .register_group(ToolGroup::new("second", true, [Tool::from_sync(EchoTool)]))
        .expect_err("duplicate tool should fail");

    assert!(matches!(err, ToolRegistryError::AlreadyExists(name) if name == "echo"));
    Ok(())
}

#[test]
fn tool_set_can_retain_only_selected_registry_groups() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    registry.register_group(ToolGroup::new("allowed", true, [Tool::from_sync(EchoTool)]))?;
    registry.register_group(ToolGroup::new(
        "blocked",
        true,
        [Tool::from_sync(OtherTool)],
    ))?;
    registry.start_all()?;

    let mut tool_set = registry.tool_set();
    tool_set.retain_registry_groups(&["allowed"]);
    let handle = tool_set.begin()?;

    let allowed = run_with_default_gate(&handle, &invocation("echo", "{}")?)?;
    assert_eq!(
        allowed,
        ToolRunOutcome::Ran {
            content: "{}".into(),
            ok: true,
        }
    );

    let blocked = run_with_default_gate(&handle, &invocation("other", "{}")?)?;
    assert_eq!(
        blocked,
        ToolRunOutcome::Ran {
            content: "tool not found: other".into(),
            ok: false,
        }
    );
    Ok(())
}

#[test]
fn tool_set_uses_registry_group_default_visibility() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    registry.register_group(ToolGroup::new("hidden", false, [Tool::from_sync(EchoTool)]))?;
    registry.start_all()?;

    let mut tool_set = registry.tool_set();
    let handle = tool_set.begin()?;
    let outcome = run_with_default_gate(&handle, &invocation("echo", "{}")?)?;
    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: "tool not found: echo".into(),
            ok: false,
        }
    );
    Ok(())
}

#[test]
fn hidden_group_is_searchable_then_loadable() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    registry.register_group(ToolGroup::new("visible", true, [Tool::from_sync(OtherTool)]))?;
    registry.register_group(ToolGroup::new("hidden", false, [Tool::from_sync(EchoTool)]))?;
    registry.start_all()?;

    let mut tool_set = registry.tool_set();
    let discovery = tool_set.discovery();

    // The hidden tool is registered but not part of the default schema surface,
    // so it is not callable yet — only surfaced through the discovery catalog.
    {
        let handle = tool_set.begin()?;
        assert_eq!(
            handle.schemas_json(),
            r#"[{"type":"function","function":{"name":"other"}}]"#
        );
        let blocked = run_with_default_gate(&handle, &invocation("echo", "{}")?)?;
        assert_eq!(
            blocked,
            ToolRunOutcome::Ran {
                content: "tool not found: echo".into(),
                ok: false,
            }
        );
    }

    let catalog = discovery.catalog();
    assert_eq!(catalog.len(), 1);
    assert_eq!(catalog[0].id, "hidden");
    assert_eq!(catalog[0].tools.len(), 1);
    assert_eq!(catalog[0].tools[0].name, "echo");
    assert_eq!(catalog[0].tools[0].description, "Echoes the normalized arguments.");

    // Loading the group queues it; the tool becomes callable after the set
    // applies pending loads.
    assert!(discovery.request_load("hidden"));
    assert!(!discovery.request_load("nope"));
    tool_set.apply_pending_tool_loads();

    let handle = tool_set.begin()?;
    let outcome = run_with_default_gate(&handle, &invocation("echo", "{}")?)?;
    assert_eq!(
        outcome,
        ToolRunOutcome::Ran {
            content: "{}".into(),
            ok: true,
        }
    );
    // Once loaded, the group drops out of the catalog.
    assert!(discovery.catalog().is_empty());
    Ok(())
}

#[test]
fn tool_set_durable_state_tracks_tool_metadata() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    assert_eq!(tool_set.generation(), 0);

    tool_set.add_group(ToolGroup::new("local", true, [Tool::from_sync(EchoTool)]))?;
    assert_eq!(tool_set.generation(), 1);

    tool_set.temporarily_disable_tool("echo".into())?;
    assert_eq!(tool_set.generation(), 2);

    tool_set.temporarily_disable_tool("echo".into())?;
    assert_eq!(tool_set.generation(), 2);

    let blob = tool_set.export_state()?;
    let state: serde_json::Value = serde_json::from_slice(&blob.bytes)?;
    assert_eq!(
        state
            .pointer("/registry_version")
            .and_then(|value| value.as_u64()),
        Some(0)
    );
    assert_eq!(
        state
            .pointer("/tools/echo/source")
            .and_then(|value| value.as_str()),
        Some("local")
    );
    assert_eq!(
        state
            .pointer("/tools/echo/state")
            .and_then(|value| value.as_str()),
        Some("temporarily_disabled")
    );
    assert!(state.pointer("/tools/echo/schema").is_none());
    Ok(())
}

#[test]
fn restored_tool_set_does_not_dirty_generation() -> Result<()> {
    let registry = Arc::new(ToolRegistry::new());
    let mut tool_set = registry.tool_set();
    tool_set.add_group(ToolGroup::new("local", true, [Tool::from_sync(EchoTool)]))?;
    tool_set.temporarily_disable_tool("echo".into())?;
    let blob = tool_set.export_state()?;

    let mut restored = registry.tool_set();
    restored.add_group(ToolGroup::new("local", true, [Tool::from_sync(EchoTool)]))?;
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
    registry.register_group(ToolGroup::new("test", true, [Tool::from_sync(EchoTool)]))?;
    registry.disable("echo")?;

    let blob = registry.export_state()?;
    let restored = <ToolRegistry as DurablePart>::restore_from_state(blob.as_slice())?;
    assert_eq!(restored.generation(), 0);

    restored.register_group(ToolGroup::new("test", true, [Tool::from_sync(EchoTool)]))?;
    assert_eq!(restored.generation(), 0);

    restored.enable("echo")?;
    assert_eq!(restored.generation(), 1);
    Ok(())
}

#[test]
fn registry_rolls_back_mutation_when_checkpoint_hook_fails() -> Result<()> {
    let registry = ToolRegistry::new();
    registry.set_checkpoint_hook(|_, _, _| {
        Err(CheckpointError::Storage(CheckpointStorageError::EmptyRoot).into())
    });

    assert!(matches!(
        registry.register_group(ToolGroup::new("test", true, [Tool::from_sync(EchoTool)])),
        Err(ToolRegistryError::Checkpoint(
            ToolRegistryCheckpointError::Coordinator(_)
        ))
    ));
    let blob = registry.export_state()?;
    let state: serde_json::Value = serde_json::from_slice(&blob.bytes)?;
    assert!(state
        .get("tools")
        .and_then(|value| value.as_object())
        .filter(|tools| tools.is_empty())
        .is_some());

    assert!(matches!(
        registry.register_group(ToolGroup::new("test", true, [Tool::from_sync(EchoTool)])),
        Err(ToolRegistryError::Checkpoint(
            ToolRegistryCheckpointError::Coordinator(_)
        ))
    ));
    Ok(())
}

#[test]
fn registry_checkpoint_hook_receives_matching_generation_and_owned_state() -> Result<()> {
    let snapshots = Arc::new(Mutex::new(Vec::new()));
    let hook_snapshots = Arc::clone(&snapshots);
    let registry = ToolRegistry::new();
    registry.set_checkpoint_hook(move |generation, state, _| {
        let owned = matches!(&state.bytes, std::borrow::Cow::Owned(_));
        hook_snapshots
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner())
            .push((generation, owned, state.bytes.into_owned()));
        Ok(())
    });

    registry.register_group(ToolGroup::new("test", true, [Tool::from_sync(EchoTool)]))?;
    registry.disable("echo")?;

    let snapshots = snapshots
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner());
    assert_eq!(snapshots.len(), 2);
    let registered_snapshot = snapshots
        .first()
        .ok_or_else(|| anyhow!("missing registered snapshot"))?;
    assert_eq!(registered_snapshot.0, 1);
    assert!(registered_snapshot.1);
    let registered: serde_json::Value = serde_json::from_slice(&registered_snapshot.2)?;
    assert_eq!(
        registered
            .pointer("/tools/echo")
            .and_then(serde_json::Value::as_bool),
        Some(true)
    );

    let disabled_snapshot = snapshots
        .get(1)
        .ok_or_else(|| anyhow!("missing disabled snapshot"))?;
    assert_eq!(disabled_snapshot.0, 2);
    assert!(disabled_snapshot.1);
    let disabled: serde_json::Value = serde_json::from_slice(&disabled_snapshot.2)?;
    assert_eq!(
        disabled
            .pointer("/tools/echo")
            .and_then(serde_json::Value::as_bool),
        Some(false)
    );
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

struct OtherTool;

impl ToolSpec for OtherTool {
    fn name(&self) -> &str {
        "other"
    }

    fn schema(&self) -> &str {
        r#"{"type":"function","function":{"name":"other"}}"#
    }
}

impl SyncToolHandler for OtherTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput> {
        if call.name() != self.name() {
            return Err(ToolError::NotFound(call.name().to_owned()).into());
        }
        Ok(ToolOutput {
            output: "other".into(),
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
    tool_set.add_group(ToolGroup::new(
        "retry",
        true,
        [Tool::from_sync(FailBeforeSuccess {
            attempts,
            retry_count,
        })],
    ))?;
    let handle = tool_set.begin()?;
    let call = invocation("retry_demo", "{}")?;
    run_with_default_gate(&handle, &call)
}

fn poll_ready<T>(future: impl Future<Output = T>) -> Result<T> {
    let mut context = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    match future.as_mut().poll(&mut context) {
        Poll::Ready(output) => Ok(output),
        Poll::Pending => Err(anyhow!("future was pending")),
    }
}
