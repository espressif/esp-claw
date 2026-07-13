//! Internal natural-language approval resolver for the orchestrator.
//!
//! This is deliberately not an agent tool. The channel user replies in free
//! text, and the orchestrator runs one short LLM/tool round to classify that text
//! into the internal [`ApprovalDecision`] it feeds back to the parked agent.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, LazyLock, Mutex, RwLock};

use claw_api::{ClawApiAsync, InitError, RetryPolicy};
use claw_interface::http::StreamingHttp;
use claw_interface::{ClawHttp, ClawTimer};
use claw_permission::{Action, PermissionDecision, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, Tool, ToolError, ToolGate, ToolGroup, ToolInvocation,
    ToolInvokeError, ToolOutput, ToolRegistry, ToolSetError, ToolSpec,
};
use serde_json::{json, Value};

use crate::agent::{
    ApprovalDecision, CompletedKind, InterruptionControl, IterationId, IterationLoop,
    IterationLoopError, IterationOutcome, IterationStep,
};
use crate::config::{ApiUsage, ClawApiManager};
use crate::event::EventSink;
use crate::orchestrator::control::DriveControl;

const APPROVAL_RESOLVER_PROMPT: &str = r#"You resolve a user's natural-language reply to one pending permission request.

You must call permission_resolve_reply exactly once.

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
    tool_metadata!("permission_resolve_reply");

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new("permission_resolve_reply", RiskClass::Safe)
    }
}

impl SyncToolHandler for ResolvePermissionReplyTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let args: Value = serde_json::from_str(call.arguments_json()).map_err(|error| {
            ToolError::InvalidArgumentsJson(format!("invalid tool arguments JSON: {error}"))
        })?;
        if !args.is_object() {
            return Err(ToolError::InvalidArgumentsJson(
                "tool arguments must be a JSON object".into(),
            )
            .into());
        }

        let Some(decision) = args
            .get("decision")
            .and_then(Value::as_str)
            .map(str::trim)
            .filter(|value| !value.is_empty())
        else {
            return Err(ToolError::InvalidArguments(
                "decision is required and must be a non-empty string".into(),
            )
            .into());
        };

        let resolution = match decision {
            "approve" => PermissionReplyResolution::Approved,
            "reject" => PermissionReplyResolution::Rejected(
                match args
                    .get("note")
                    .and_then(Value::as_str)
                    .map(str::trim)
                    .filter(|value| !value.is_empty())
                {
                    Some(note) => note,
                    None => DEFAULT_REJECTION,
                }
                .to_string(),
            ),
            "clarify" => PermissionReplyResolution::Clarify(
                match args
                    .get("message")
                    .and_then(Value::as_str)
                    .map(str::trim)
                    .filter(|value| !value.is_empty())
                {
                    Some(message) => message,
                    None => DEFAULT_CLARIFICATION,
                }
                .to_string(),
            ),
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
    api_manager: &Arc<RwLock<ClawApiManager>>,
    summary: &str,
    user_reply: &str,
    control: &DriveControl,
) -> Result<PermissionReplyResolution, ApprovalResolverError>
where
    H: ClawHttp + StreamingHttp + Default + 'static,
    Timer: ClawTimer + Default + 'static,
{
    let mut llm = ClawApiAsync::<H, Timer>::new(H::default(), Timer::default());
    // Approval resolution runs on the root agent's config (its explicit binding,
    // else the default). With no binding, the request reports NotConfigured.
    if let Some(config) = api_manager
        .read()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
        .get_api(ApiUsage::RootAgent)
    {
        llm.set_config(config)?;
    }
    let resolution = Arc::new(Mutex::new(None));
    // Approval classification uses an isolated local tool set.
    let mut tools = APPROVAL_TOOL_PARENT.tool_set();
    tools.add_group(ToolGroup::new(
        "permission",
        true,
        [Tool::from_sync(ResolvePermissionReplyTool::new(
            Arc::clone(&resolution),
        ))],
    ))?;
    let tools = tools.begin()?;
    let gate = AllowGate;
    let resolver_control = ApprovalResolverControl::new();
    let cancel_handle = resolver_control.cancel_handle();
    control.set_cancel_hook(move || {
        cancel_handle.store(true, Ordering::Release);
    });

    let messages = json!([
        {
            "role": "user",
            "content": format!(
                "Pending permission request:\n{summary}\n\nUser reply:\n{user_reply}"
            )
        }
    ]);
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
        system_prompt: APPROVAL_RESOLVER_PROMPT,
        messages: &messages,
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
            CompletedKind::PlainText(text) => {
                let text = text.text.trim();
                Ok(PermissionReplyResolution::Clarify(
                    if text.is_empty() {
                        DEFAULT_CLARIFICATION
                    } else {
                        text
                    }
                    .to_string(),
                ))
            }
        },
    }
}
