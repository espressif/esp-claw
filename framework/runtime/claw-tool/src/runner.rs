//! The tool-execution boundary: per-call gating and dispatch, isolated from the
//! caller's orchestration (preemption checkpoints, tracing spans, and message
//! assembly stay in the iteration loop that drives this).
//!
//! One call passes through three stages: **soft-hide** gating (is the tool
//! permitted this phase?), **permission** gating (does the policy allow / ask /
//! deny it?), then **execution**. The runner returns a neutral [`CallOutcome`]
//! the caller turns into a tool message and a per-call record.
//!
//! ## Async / concurrency boundary
//!
//! Today the loop still preserves model order. The shape here — *classify → gate
//! → execute*, with a per-tool [`concurrent`](crate::ToolSet::concurrent) hint
//! surfaced via [`is_concurrent`](ToolRunner::is_concurrent) — is the boundary a
//! future batch scheduler grows into: side-effect-free `concurrent` calls awaited
//! together, serializing ones run in order. Keeping that decision *here* means the
//! caller does not change when concurrency lands. The per-call retry budget
//! (`invoke_with_retries`) is the other half of that boundary. See the workspace
//! `ROADMAP.md` ("Async, fair-scheduling tool runner") for the planned backoff,
//! preemption, and fair-scheduling work.

use claw_permission::{Action, PermissionDecision};

use crate::handler::{ToolError, ToolInvocation, ToolInvokeError, ToolOutput};
use crate::set::ToolSet;

/// The permission boundary the runner consults before executing a classified call.
///
/// Implemented by the agent layer that owns the permission policy, the grant
/// store, and the acting agent's identity — the runner stays agnostic of all
/// three and only asks "what is the verdict for this action?".
pub trait ToolGate {
    /// The permission verdict for the call described by `action`.
    fn decide(&self, action: &Action) -> PermissionDecision;
}

/// What an `Ask` decision needs the agent layer to remember to resolve it: the
/// human-facing `summary` and the action `signature` to grant/deny against.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApprovalNeeded {
    /// Shown to the approver (the policy's reason).
    pub summary: String,
    /// The action signature a grant/denial is recorded under.
    pub signature: String,
}

/// The runner's verdict for one call, ready for the caller to render.
pub struct CallOutcome {
    /// The tool-message content handed back to the model.
    pub content: String,
    /// Whether the call succeeded (false for blocked / denied / asked / a tool
    /// that ran and reported failure).
    pub ok: bool,
    /// True when refused by soft-hide gating (drives the retry-then-fail policy).
    pub blocked: bool,
    /// `Some` when the permission policy asked for human approval; the tool did
    /// not run and the agent layer must raise + later resolve the request.
    pub approval: Option<ApprovalNeeded>,
}

impl CallOutcome {
    /// A plain executed result.
    fn ran(content: String, ok: bool) -> Self {
        Self {
            content,
            ok,
            blocked: false,
            approval: None,
        }
    }
}

/// Gates and executes individual tool calls for one iteration. Cheap to build per
/// batch; borrows the tool set (which carries its own soft-hide allow-set) and the
/// optional permission gate.
pub struct ToolRunner<'a> {
    tools: &'a ToolSet,
    gate: Option<&'a dyn ToolGate>,
}

impl<'a> ToolRunner<'a> {
    /// Build a runner over `tools` and the permission `gate` (`None` = no
    /// permission layer; every call that passes soft-hide runs). Soft-hide gating
    /// is read from `tools` itself (see [`ToolSet::set_active_tools`]).
    pub fn new(tools: &'a ToolSet, gate: Option<&'a dyn ToolGate>) -> Self {
        Self { tools, gate }
    }

    /// Whether `name`'s tool may run concurrently (the async scheduling hint; unknown
    /// tools are treated as serializing).
    ///
    /// Reserved for the future async runner (see the module docs): today every
    /// call runs in order, so nothing consults this yet.
    #[allow(dead_code)]
    pub fn is_concurrent(&self, name: &str) -> bool {
        self.tools.concurrent(name).unwrap_or(false)
    }

    /// Gate `call` and, if permitted, execute it.
    ///
    /// Infallible: every path — soft-hide block, permission deny / ask, a tool
    /// error, an unknown tool, or a clean run — produces a [`CallOutcome`] the
    /// caller turns into a matched tool message. Tool-layer failures are handed
    /// back as `ok = false` content the model can self-correct from, never as an
    /// error that aborts the iteration.
    pub fn run_one(&self, call: &ToolInvocation<'_>) -> CallOutcome {
        if let Some(outcome) = self.outcome_before_execution(call) {
            return outcome;
        }

        // Execute — optional per-call automatic re-invocation when the handler
        // returns a non-empty [`ToolRetryCount`]; otherwise surface a tool message.
        match invoke_with_retries(self.tools, call) {
            Ok(ToolOutput { output, ok }) => CallOutcome::ran(output, ok),
            Err(error) => invoke_failure_to_outcome(call, error),
        }
    }

    /// Async counterpart of [`run_one`](Self::run_one).
    ///
    /// The gating path stays synchronous policy work; only tool execution yields.
    /// This lets the agent layer switch to an async runner without changing the
    /// model-facing outcomes or permission semantics.
    pub async fn run_one_async(&self, call: &ToolInvocation<'_>) -> CallOutcome {
        if let Some(outcome) = self.outcome_before_execution(call) {
            return outcome;
        }

        match invoke_with_retries_async(self.tools, call).await {
            Ok(ToolOutput { output, ok }) => CallOutcome::ran(output, ok),
            Err(error) => invoke_failure_to_outcome(call, error),
        }
    }

    /// Return the final outcome for calls stopped before tool execution, or
    /// `None` when execution should proceed.
    fn outcome_before_execution(&self, call: &ToolInvocation<'_>) -> Option<CallOutcome> {
        // 1. Soft-hide gating: the schema superset reached the model, but a tool
        //    the set does not currently allow must not run this phase.
        if !self.tools.is_allowed(call.name) {
            return Some(CallOutcome {
                content: blocked_tool_message(call.name),
                ok: false,
                blocked: true,
                approval: None,
            });
        }

        // 2. Permission gating: classify the call and ask the policy. An unknown
        //    tool cannot be classified — it falls through to dispatch, which
        //    returns the NotFound error.
        if let (Some(gate), Some(action)) = (self.gate, self.tools.classify(call)) {
            match gate.decide(&action) {
                PermissionDecision::Allow => {}
                PermissionDecision::Deny { reason } => {
                    return Some(CallOutcome::ran(
                        denied_tool_message(call.name, &reason),
                        false,
                    ));
                }
                PermissionDecision::Ask { reason } => {
                    return Some(CallOutcome {
                        content: ask_tool_message(call.name),
                        ok: false,
                        blocked: false,
                        approval: Some(ApprovalNeeded {
                            summary: reason,
                            signature: action.signature(),
                        }),
                    });
                }
            }
        }

        None
    }
}

/// Invoke `call`, re-running the same invocation when the handler supplies a
/// [`ToolRetryCount`]. Intermediate failures are not surfaced to the model; the
/// final failure is reduced to its [`ToolError`] (the budget is spent).
fn invoke_with_retries(
    tools: &ToolSet,
    call: &ToolInvocation<'_>,
) -> Result<ToolOutput, ToolError> {
    let mut extra_attempts = 0u32;
    loop {
        match tools.invoke(call) {
            Ok(output) => return Ok(output),
            Err(ToolInvokeError { error, retries }) => {
                if extra_attempts < retries.get() {
                    extra_attempts = extra_attempts.saturating_add(1);
                    continue;
                }
                return Err(error);
            }
        }
    }
}

/// Async form of [`invoke_with_retries`].
async fn invoke_with_retries_async(
    tools: &ToolSet,
    call: &ToolInvocation<'_>,
) -> Result<ToolOutput, ToolError> {
    let mut extra_attempts = 0u32;
    loop {
        match tools.invoke_async(call).await {
            Ok(output) => return Ok(output),
            Err(ToolInvokeError { error, retries }) => {
                if extra_attempts < retries.get() {
                    extra_attempts = extra_attempts.saturating_add(1);
                    continue;
                }
                return Err(error);
            }
        }
    }
}

fn invoke_failure_to_outcome(call: &ToolInvocation<'_>, error: ToolError) -> CallOutcome {
    CallOutcome::ran(error.model_message(call.name), false)
}

/// Content handed to the model when soft-hide gating refuses a call. Worded so
/// the model treats it as a policy restriction (not a transient failure to
/// retry) and switches to a permitted tool.
fn blocked_tool_message(name: &str) -> String {
    let name = display_name(name);
    format!(
        "Tool \"{name}\" is not available in the current phase and was not executed. \
         This is a policy restriction, not a transient error: do not retry it. \
         Use one of the tools listed as available in the latest instructions instead."
    )
}

/// Content handed to the model when the permission policy denies a call.
fn denied_tool_message(name: &str, reason: &str) -> String {
    let name = display_name(name);
    format!(
        "Tool \"{name}\" was denied by policy and was not executed: {reason} \
         This is a policy restriction, not a transient error: do not retry it."
    )
}

/// Content handed to the model when the permission policy asks for approval. The
/// tool did not run; a human decision is pending and the model should wait.
fn ask_tool_message(name: &str) -> String {
    let name = display_name(name);
    format!(
        "Tool \"{name}\" requires human approval and was not executed yet. \
         A decision has been requested; wait for it before retrying."
    )
}

/// The display form of a (possibly empty) tool name.
fn display_name(name: &str) -> &str {
    if name.is_empty() {
        "(null)"
    } else {
        name
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::expect_used)]
mod tests {
    use super::*;
    use crate::handler::{
        tool_invoke_err_with_retries, AsyncToolHandler, Tool, ToolFuture, ToolHandler,
        ToolInvokeError, ToolOutput, ToolRetryCount,
    };
    use crate::set::{AllowedTools, ToolGroup};
    use claw_permission::{Action, RiskClass};
    use core::future::Future;
    use core::task::{Context, Poll};
    use std::sync::{mpsc, Arc, Mutex};
    use std::task::{Wake, Waker};
    use std::time::Duration;

    /// A tool that records nothing and returns a fixed result; risk-classified so
    /// the permission path can be exercised.
    struct RiskyTool;
    impl ToolHandler for RiskyTool {
        fn name(&self) -> &str {
            "risky"
        }
        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"risky"}}"#
        }
        fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
            Action::new("risky", RiskClass::High)
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: "ran".into(),
                ok: true,
            })
        }
    }

    /// A gate scripted with one fixed decision.
    struct FixedGate(PermissionDecision);
    impl ToolGate for FixedGate {
        fn decide(&self, _action: &Action) -> PermissionDecision {
            self.0.clone()
        }
    }

    fn tools() -> ToolSet {
        ToolSet::from_groups([ToolGroup::new("g", [Tool::new(RiskyTool)])]).unwrap()
    }

    fn call() -> ToolInvocation<'static> {
        ToolInvocation {
            id: Some("t1"),
            name: "risky",
            arguments_json: "{}",
        }
    }

    struct NoopWake;
    impl Wake for NoopWake {
        fn wake(self: Arc<Self>) {}
    }

    fn block_on<F: Future>(future: F) -> F::Output {
        let mut future = Box::pin(future);
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);
        loop {
            if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
                return value;
            }
        }
    }

    #[test]
    fn allow_runs_the_tool() {
        let tools = tools();
        let gate = FixedGate(PermissionDecision::Allow);
        let runner = ToolRunner::new(&tools, Some(&gate));
        let outcome = runner.run_one(&call());
        assert_eq!(outcome.content, "ran");
        assert!(outcome.ok);
        assert!(outcome.approval.is_none());
    }

    #[test]
    fn deny_refuses_without_running() {
        let tools = tools();
        let gate = FixedGate(PermissionDecision::Deny {
            reason: "no".into(),
        });
        let runner = ToolRunner::new(&tools, Some(&gate));
        let outcome = runner.run_one(&call());
        assert!(!outcome.ok);
        assert!(outcome.approval.is_none());
        assert!(outcome.content.contains("denied by policy"));
    }

    #[test]
    fn ask_yields_approval_needed_without_running() {
        let tools = tools();
        let gate = FixedGate(PermissionDecision::Ask {
            reason: "confirm".into(),
        });
        let runner = ToolRunner::new(&tools, Some(&gate));
        let outcome = runner.run_one(&call());
        assert!(!outcome.ok);
        let approval = outcome.approval.expect("approval needed");
        assert_eq!(approval.summary, "confirm");
        assert_eq!(approval.signature, "risky");
    }

    #[test]
    fn soft_hide_blocks_before_permission() {
        let mut tools = tools();
        tools.set_active_tools(AllowedTools::new(["other"]));
        // Even an Allow gate never runs: soft-hide refuses first.
        let gate = FixedGate(PermissionDecision::Allow);
        let runner = ToolRunner::new(&tools, Some(&gate));
        let outcome = runner.run_one(&call());
        assert!(outcome.blocked);
        assert!(!outcome.ok);
    }

    #[test]
    fn no_gate_runs_normally() {
        let tools = tools();
        let runner = ToolRunner::new(&tools, None);
        let outcome = runner.run_one(&call());
        assert_eq!(outcome.content, "ran");
        assert!(outcome.ok);
    }

    #[test]
    fn async_runner_offloads_sync_tool_work() {
        struct BlockingTool {
            started: Mutex<Option<mpsc::Sender<()>>>,
            release: Mutex<mpsc::Receiver<()>>,
        }

        impl ToolHandler for BlockingTool {
            fn name(&self) -> &str {
                "blocking"
            }

            fn schema(&self) -> &str {
                r#"{"type":"function","function":{"name":"blocking","parameters":{"type":"object","properties":{}}}}"#
            }

            fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
                if let Some(started) = self.started.lock().unwrap().take() {
                    let _ = started.send(());
                }
                let _ = self.release.lock().unwrap().recv();
                Ok(ToolOutput {
                    output: "unblocked".into(),
                    ok: true,
                })
            }
        }

        let (started_tx, started_rx) = mpsc::channel();
        let (release_tx, release_rx) = mpsc::channel();
        let tools = ToolSet::new([Tool::new(BlockingTool {
            started: Mutex::new(Some(started_tx)),
            release: Mutex::new(release_rx),
        })])
        .unwrap();
        let runner = ToolRunner::new(&tools, None);
        let invocation = ToolInvocation {
            id: Some("t1"),
            name: "blocking",
            arguments_json: "{}",
        };
        let mut future = Box::pin(runner.run_one_async(&invocation));
        let waker = Waker::from(Arc::new(NoopWake));
        let mut context = Context::from_waker(&waker);

        assert!(matches!(future.as_mut().poll(&mut context), Poll::Pending));
        started_rx
            .recv_timeout(Duration::from_secs(1))
            .expect("tool should start on the tool executor");
        release_tx.send(()).expect("release blocking tool");

        let outcome = block_on(future);
        assert!(outcome.ok);
        assert_eq!(outcome.content, "unblocked");
    }

    #[test]
    fn invalid_arguments_json_becomes_recoverable_tool_message() {
        struct JsonTool;
        impl ToolHandler for JsonTool {
            fn name(&self) -> &str {
                "json_tool"
            }
            fn schema(&self) -> &str {
                r#"{
                    "type": "function",
                    "function": {
                        "name": "json_tool",
                        "parameters": {
                            "type": "object",
                            "properties": { "name": { "type": "string" } },
                            "required": ["name"]
                        }
                    }
                }"#
            }
            fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
                Ok(ToolOutput {
                    output: "ran".into(),
                    ok: true,
                })
            }
        }
        let tools = ToolSet::new([Tool::new(JsonTool)]).unwrap();
        let runner = ToolRunner::new(&tools, None);
        let outcome = runner.run_one(&ToolInvocation {
            id: Some("t1"),
            name: "json_tool",
            arguments_json: "[",
        });
        assert!(!outcome.ok);
        assert!(outcome.content.contains("not valid JSON"));
    }

    #[test]
    fn async_runner_executes_async_tool() {
        struct AsyncTool;
        impl AsyncToolHandler for AsyncTool {
            fn name(&self) -> &str {
                "async_tool"
            }

            fn schema(&self) -> &str {
                r#"{"type":"function","function":{"name":"async_tool","parameters":{"type":"object","properties":{}}}}"#
            }

            fn invoke_async<'a>(&'a self, _call: &'a ToolInvocation<'_>) -> ToolFuture<'a> {
                Box::pin(async {
                    Ok(ToolOutput {
                        output: "async ran".into(),
                        ok: true,
                    })
                })
            }
        }

        let tools = ToolSet::new([Tool::new_async(AsyncTool)]).unwrap();
        let runner = ToolRunner::new(&tools, None);
        let outcome = block_on(runner.run_one_async(&ToolInvocation {
            id: Some("t1"),
            name: "async_tool",
            arguments_json: "{}",
        }));

        assert!(outcome.ok);
        assert_eq!(outcome.content, "async ran");
    }

    #[test]
    fn per_call_retry_reinvokes_before_surfacing_error() {
        use std::sync::atomic::{AtomicU32, Ordering};

        struct FlakyTool {
            calls: AtomicU32,
        }
        impl ToolHandler for FlakyTool {
            fn name(&self) -> &str {
                "flaky"
            }
            fn schema(&self) -> &str {
                r#"{"type":"function","function":{"name":"flaky","parameters":{"type":"object","properties":{}}}}"#
            }
            fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
                if self.calls.fetch_add(1, Ordering::Relaxed) == 0 {
                    Err(tool_invoke_err_with_retries(
                        ToolError::invoke_rejected("transient"),
                        ToolRetryCount::extra(1),
                    ))
                } else {
                    Ok(ToolOutput {
                        output: "ok after retry".into(),
                        ok: true,
                    })
                }
            }
        }

        let tools = ToolSet::new([Tool::new(FlakyTool {
            calls: AtomicU32::new(0),
        })])
        .unwrap();
        let runner = ToolRunner::new(&tools, None);
        let outcome = runner.run_one(&ToolInvocation {
            id: Some("t1"),
            name: "flaky",
            arguments_json: "{}",
        });
        assert!(outcome.ok);
        assert_eq!(outcome.content, "ok after retry");
    }
}
