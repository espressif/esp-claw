pub mod bake;
mod registry;
mod runner;
mod set;
#[allow(clippy::module_inception)]
mod tool;
mod validate;

pub use registry::{ToolRegistry, ToolRegistryError, ToolRegistryVersion};
pub use runner::{ApprovalNeeded, ToolGate, ToolRunOutcome, ToolRunner};
pub use set::{ToolName, ToolSet, ToolSetError, ToolSetHandle};
pub use tool::{
    AsyncToolHandler, RawToolInvocation, RetryCount, SyncToolHandler, Tool, ToolError, ToolFuture,
    ToolInvocation, ToolInvokeError, ToolOutput, ToolResult, ToolSpec,
};

#[cfg(test)]
mod tests {
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::Arc;
    use std::task::Wake;

    use anyhow::{anyhow, Result};

    use super::{
        RawToolInvocation, SyncToolHandler, Tool, ToolError, ToolInvocation, ToolOutput,
        ToolRegistry, ToolResult, ToolRunOutcome, ToolRunner, ToolSetHandle, ToolSpec,
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

    struct NoopWake;

    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn invocation(
        name: &'static str,
        arguments_json: &'static str,
    ) -> Result<ToolInvocation<'static>> {
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

    fn poll_ready<T>(future: impl Future<Output = T>) -> Result<T> {
        let waker = std::task::Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        let mut future = std::pin::pin!(future);
        match future.as_mut().poll(&mut context) {
            Poll::Ready(output) => Ok(output),
            Poll::Pending => Err(anyhow!("future was pending")),
        }
    }
}
