//! Internal natural-language approval resolver for the orchestrator.
//!
//! This is deliberately not an agent tool. The channel user replies in free
//! text, and the orchestrator runs one short LLM/tool round to classify that text
//! into the internal [`ApprovalDecision`] it feeds back to the parked agent.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, LazyLock, Mutex};

use claw_api::{ClawApiAsync, ClawApiConfig, InitError, RetryPolicy};
use claw_interface::{ClawHttp, ClawTimer};
use claw_permission::{Action, PermissionDecision, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolGate, ToolInvocation, ToolInvokeError,
    ToolOutput, ToolRegistry, ToolSetError, ToolSpec,
};
use serde_json::{json, Value};

use crate::agent::{
    ApprovalDecision, ChatMessages, CompletedKind, InterruptionControl, IterationId, IterationLoop,
    IterationLoopError, IterationOutcome, IterationStep, SystemPrompt,
};
use crate::event::EventSink;
use crate::orchestrator::control::DriveControl;

const APPROVAL_RESOLVER_PROMPT: &str = r#"You resolve a user's natural-language reply to one pending permission request.

You must call resolve_permission_reply exactly once.

Use:
- decision="approve" only when the user clearly allows the pending request.
- decision="reject" when the user clearly refuses, objects, or asks not to proceed.
- decision="clarify" when the reply is a question, is ambiguous, or asks for more information before deciding.

Do not answer the user directly. The tool result is the only output."#;

const DEFAULT_CLARIFICATION: &str = "Please clearly reply with approval or rejection.";
const DEFAULT_REJECTION: &str = "rejected";
static APPROVAL_TOOL_PARENT: LazyLock<Arc<ToolRegistry>> =
    LazyLock::new(|| Arc::new(ToolRegistry::new()));

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum PermissionReplyResolution {
    Approved,
    Rejected(String),
    Clarify(String),
}

impl PermissionReplyResolution {
    pub(crate) fn into_decision(self) -> Option<ApprovalDecision> {
        match self {
            Self::Approved => Some(ApprovalDecision::Approved),
            Self::Rejected(reason) => Some(ApprovalDecision::Rejected(reason)),
            Self::Clarify(_) => None,
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub(crate) enum ApprovalResolverError {
    #[error("approval resolver was cancelled")]
    Cancelled,
    #[error("failed to initialize approval resolver LLM: {0}")]
    Init(#[from] InitError),
    #[error(transparent)]
    ToolSet(#[from] ToolSetError),
    #[error(transparent)]
    Iteration(#[from] IterationLoopError),
}

struct ApprovalResolverControl {
    interrupt: Arc<AtomicBool>,
}

impl ApprovalResolverControl {
    fn new() -> Self {
        Self {
            interrupt: Arc::new(AtomicBool::new(false)),
        }
    }

    fn cancel_handle(&self) -> Arc<AtomicBool> {
        Arc::clone(&self.interrupt)
    }
}

impl InterruptionControl for ApprovalResolverControl {
    fn interrupt_flag(&self) -> &Arc<AtomicBool> {
        &self.interrupt
    }
}

struct ResolvePermissionReplyTool {
    resolution: Arc<Mutex<Option<PermissionReplyResolution>>>,
}

impl ResolvePermissionReplyTool {
    fn new(resolution: Arc<Mutex<Option<PermissionReplyResolution>>>) -> Self {
        Self { resolution }
    }
}

impl ToolSpec for ResolvePermissionReplyTool {
    tool_metadata!("resolve_permission_reply");

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new("resolve_permission_reply", RiskClass::Safe)
    }
}

impl SyncToolHandler for ResolvePermissionReplyTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args = parse_arguments(call.arguments_json())?;
        let decision = string_field(&args, "decision");
        let resolution = match decision.trim() {
            "approve" => PermissionReplyResolution::Approved,
            "reject" => PermissionReplyResolution::Rejected(non_empty_field(
                &args,
                "note",
                DEFAULT_REJECTION,
            )),
            "clarify" => PermissionReplyResolution::Clarify(non_empty_field(
                &args,
                "message",
                DEFAULT_CLARIFICATION,
            )),
            other => {
                return Err(ToolError::InvalidArguments(format!(
                    "decision must be approve|reject|clarify, got '{other}'"
                ))
                .into())
            }
        };

        *self
            .resolution
            .lock()
            .unwrap_or_else(|poison| poison.into_inner()) = Some(resolution);
        Ok(ToolOutput {
            output: "approval reply resolved".to_string(),
            ok: true,
        })
    }
}

struct AllowGate;

impl ToolGate for AllowGate {
    fn decide(&self, _action: &Action) -> PermissionDecision {
        PermissionDecision::Allow
    }
}

pub(crate) async fn resolve_permission_reply<H, Timer>(
    llm_config: ClawApiConfig,
    summary: &str,
    user_reply: &str,
    control: &DriveControl,
) -> Result<PermissionReplyResolution, ApprovalResolverError>
where
    H: ClawHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let mut llm = ClawApiAsync::<H, Timer>::init_default(llm_config)?;
    let resolution = Arc::new(Mutex::new(None));
    // Approval classification uses an isolated local tool set.
    let mut tools = APPROVAL_TOOL_PARENT.tool_set();
    tools.add_tool(Tool::from_sync(ResolvePermissionReplyTool::new(
        Arc::clone(&resolution),
    )))?;
    let tools = tools.begin()?;
    let gate = AllowGate;
    let resolver_control = ApprovalResolverControl::new();
    let cancel_handle = resolver_control.cancel_handle();
    control.set_cancel_hook(move || {
        cancel_handle.store(true, Ordering::Release);
    });

    let messages = approval_messages(summary, user_reply);
    let reminders: [Value; 0] = [];
    // The approval resolver is an internal one-shot, not a visible root iteration,
    // so its iteration events are dropped.
    let events = EventSink::disabled();
    let outcome = IterationLoop {
        llm: &mut llm,
        interruption: &resolver_control,
        retry: RetryPolicy::none(),
        events: &events,
    }
    .run(IterationStep {
        iteration_id: IterationId(1),
        system_prompt: SystemPrompt(APPROVAL_RESOLVER_PROMPT),
        messages: ChatMessages(&messages),
        reminders: &reminders,
        tools: &tools,
        gate: &gate,
    })
    .await;
    control.clear_cancel_hook();

    match outcome? {
        IterationOutcome::Preempted(_) => Err(ApprovalResolverError::Cancelled),
        IterationOutcome::Completed(completed) => match completed.kind {
            CompletedKind::Tools(_) => resolution
                .lock()
                .unwrap_or_else(|poison| poison.into_inner())
                .clone()
                .ok_or_else(|| IterationLoopError::MalformedAssistantMessage.into()),
            CompletedKind::PlainText(text) => Ok(PermissionReplyResolution::Clarify(
                non_empty_str(&text.text, DEFAULT_CLARIFICATION),
            )),
        },
    }
}

fn approval_messages(summary: &str, user_reply: &str) -> Value {
    json!([
        {
            "role": "user",
            "content": format!(
                "Pending permission request:\n{summary}\n\nUser reply:\n{user_reply}"
            )
        }
    ])
}

fn parse_arguments(arguments_json: &str) -> Result<Value, ToolInvokeError> {
    if arguments_json.trim().is_empty() {
        return Ok(Value::Object(Default::default()));
    }
    serde_json::from_str(arguments_json).map_err(|error| {
        ToolError::InvalidArgumentsJson(format!("invalid tool arguments JSON: {error}")).into()
    })
}

fn string_field(value: &Value, key: &str) -> String {
    value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string()
}

fn non_empty_field(value: &Value, key: &str, default: &str) -> String {
    non_empty_str(&string_field(value, key), default)
}

fn non_empty_str(value: &str, default: &str) -> String {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        default.to_string()
    } else {
        trimmed.to_string()
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use claw_tool::RawToolInvocation;

    fn resolve_from_tool(arguments_json: &str) -> Result<PermissionReplyResolution, ToolError> {
        let resolution = Arc::new(Mutex::new(None));
        let tool = ResolvePermissionReplyTool::new(Arc::clone(&resolution));
        let call = ToolInvocation::try_from(RawToolInvocation {
            id: Some("call-1"),
            name: "resolve_permission_reply",
            arguments_json,
        })
        .unwrap();
        tool.invoke(&call).map_err(|error| error.error)?;
        let resolved = resolution.lock().unwrap().clone().unwrap();
        Ok(resolved)
    }

    #[test]
    fn resolver_tool_maps_approve_and_reject() {
        assert_eq!(
            resolve_from_tool(r#"{"decision":"approve"}"#).unwrap(),
            PermissionReplyResolution::Approved
        );
        assert_eq!(
            resolve_from_tool(r#"{"decision":"reject","note":"not now"}"#).unwrap(),
            PermissionReplyResolution::Rejected("not now".to_string())
        );
    }

    #[test]
    fn resolver_tool_maps_clarify_and_rejects_unknown_decision() {
        assert_eq!(
            resolve_from_tool(r#"{"decision":"clarify","message":"please decide"}"#).unwrap(),
            PermissionReplyResolution::Clarify("please decide".to_string())
        );
        assert!(matches!(
            resolve_from_tool(r#"{"decision":"maybe"}"#).unwrap_err(),
            ToolError::InvalidArguments(_)
        ));
    }
}
