//! The agent's built-in tools — one [`ToolHandler`](claw_tool::ToolHandler) per
//! file — and the small seams they share.
//!
//! Internal tools are model-callable like any other tool, but instead of
//! returning a result to the conversation they steer the *agent graph*. A tool
//! handler only has `&self`, so it cannot touch state directly — it routes
//! through one of two seams:
//! - **self-affecting** control (`end_conversation`) pushes a [`ControlSignal`]
//!   onto the agent's own [`ControlSink`], drained each tick;
//! - **graph-affecting** actions (`spawn_subagent`, `list_subagents`,
//!   `watch_subagent`, `delete_subagent`, `respond_to_approval`) call the
//!   [`AgentContext`](crate::agent::graph::AgentContext) façade, which emits a
//!   [`GraphEffect`](crate::agent::graph::GraphEffect) (or reads a snapshot) via
//!   the [`GraphHost`](crate::agent::graph::GraphHost) — applied by the
//!   orchestrator instance at a borrow-safe point.
//!
//! Human approval is **not** a tool: it is raised by the permission layer (an
//! `Ask` decision in `base_agent`), not requested by the model. Only the
//! root-side `respond_to_approval` (the human's verdict) remains a tool.
//!
//! Which tools an agent actually gets is a build-time knob: `spawn_subagent` and
//! its siblings only when its manifest enables spawning, `respond_to_approval`
//! only for a session root.
//!
//! Keeping these here means the iteration loop stays fully agnostic: it runs them
//! like ordinary tools and never learns their meaning.

mod delete_subagent;
mod end_conversation;
mod list_spawnable_agents;
mod list_subagents;
mod respond_to_approval;
mod spawn_subagent;
mod watch_subagent;

use std::collections::VecDeque;
use std::sync::{Arc, Mutex};

use claw_permission::Resource;
use claw_tool::{Tool, ToolError, ToolGroup, ToolInvocation};
use serde_json::Value;

use crate::agent::graph::{AgentContext, AgentSnapshot, SpawnPolicy};

use delete_subagent::DeleteSubagentTool;
use end_conversation::EndConversationTool;
use list_spawnable_agents::ListSpawnableAgentsTool;
use list_subagents::ListSubagentsTool;
use respond_to_approval::RespondToApprovalTool;
use spawn_subagent::SpawnSubagentTool;
use watch_subagent::WatchSubagentTool;

/// Group label for the agent's built-in tools (provenance only).
pub(crate) const INTERNAL_TOOL_GROUP: &str = "agent";

/// Group label for the subagent-management tools (provenance only).
pub(crate) const SUBAGENT_TOOL_GROUP: &str = "subagents";

/// Group label for the approval-response tool (provenance only).
pub(crate) const APPROVAL_TOOL_GROUP: &str = "approval";

// -- Self-control seam ------------------------------------------------------

/// A signal an internal tool raises for the agent to act on next tick.
///
/// This is *internal*: it is not part of the public `AgentCommand` surface, so a
/// caller cannot forge an end-of-conversation.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum ControlSignal {
    /// The agent decided it is done; carries its closing message.
    EndConversation { final_message: String },
}

/// The shared queue internal tools push [`ControlSignal`]s onto.
///
/// The agent owns one; each internal tool handler holds a clone. A `Mutex`
/// (not a bare cell) because [`ToolHandler`](claw_tool::ToolHandler) is
/// `Send + Sync`; contention is nil in the single-driver-thread model.
pub(crate) type ControlSink = Arc<Mutex<VecDeque<ControlSignal>>>;

/// Push `signal` onto `sink`, recovering a poisoned lock (the queue is plain data).
fn push(sink: &ControlSink, signal: ControlSignal) {
    sink.lock()
        .unwrap_or_else(|poison| poison.into_inner())
        .push_back(signal);
}

// -- Shared argument / rendering helpers -------------------------------------

/// Read one string argument out of a tool call, or `""` when it is absent.
///
/// # Errors
///
/// [`ToolError::InvalidArgumentsJson`] if the arguments are present but not valid JSON —
/// a malformed call is surfaced, not swallowed.
pub(crate) fn string_argument(arguments_json: &str, key: &str) -> Result<String, ToolError> {
    if arguments_json.trim().is_empty() {
        return Ok(String::new());
    }
    let value: Value = serde_json::from_str(arguments_json).map_err(|error| {
        ToolError::InvalidArgumentsJson(format!("invalid tool arguments JSON: {error}"))
    })?;
    Ok(value
        .get(key)
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_string())
}

/// Best-effort [`Resource::Agent`] for a tool call's `agent` argument, for
/// classification only — a missing/malformed id just yields `None` (the verb
/// alone still classifies; `invoke` is where a bad id is reported).
fn agent_resource(call: &ToolInvocation<'_>) -> Option<Resource> {
    let raw = string_argument(call.arguments_json, "agent").ok()?;
    let trimmed = raw.trim();
    (!trimmed.is_empty()).then(|| Resource::Agent(trimmed.to_string()))
}

/// Render a snapshot as a compact JSON object for the model to read.
fn snapshot_json(snapshot: &AgentSnapshot) -> Value {
    serde_json::json!({
        "agent": snapshot.id.to_string(),
        "kind": snapshot.kind.as_str(),
        "name": snapshot.name,
        "parent": snapshot.parent.map(|parent| parent.to_string()),
        "depth": snapshot.depth,
        "status": snapshot.status.as_str(),
        "termination": snapshot.termination.as_str(),
    })
}

// -- Group builders ---------------------------------------------------------

/// Build the agent's built-in tool group over a control sink.
pub(crate) fn internal_tool_group(sink: ControlSink) -> ToolGroup {
    ToolGroup::new(
        INTERNAL_TOOL_GROUP,
        [Tool::new(EndConversationTool::new(sink))],
    )
}

/// Build the subagent-management tool group, all scoped by the context's agent
/// (or, for `list_spawnable_agents`, by that agent's spawn `policy`):
/// - `list_spawnable_agents` — the menu of kinds this agent may spawn;
/// - `spawn_subagent` — create a child (restricted to `policy`'s allowed kinds);
/// - `list_subagents` — enumerate this agent's subtree;
/// - `watch_subagent` — snapshot one descendant;
/// - `delete_subagent` — remove one descendant (and its subtree).
pub(crate) fn subagent_tool_group(context: Arc<AgentContext>, policy: SpawnPolicy) -> ToolGroup {
    ToolGroup::new(
        SUBAGENT_TOOL_GROUP,
        [
            Tool::new(ListSpawnableAgentsTool::new(policy.clone())),
            Tool::new(SpawnSubagentTool::new(Arc::clone(&context), policy)),
            Tool::new(ListSubagentsTool::new(Arc::clone(&context))),
            Tool::new(WatchSubagentTool::new(Arc::clone(&context))),
            Tool::new(DeleteSubagentTool::new(context)),
        ],
    )
}

/// Build the approval-response tool group: a `respond_to_approval` tool that
/// reports verdicts through `context`.
pub(crate) fn respond_to_approval_tool_group(context: Arc<AgentContext>) -> ToolGroup {
    ToolGroup::new(
        APPROVAL_TOOL_GROUP,
        [Tool::new(RespondToApprovalTool::new(context))],
    )
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
pub(crate) mod test_support {
    //! Shared tool test helpers (the graph doubles live in
    //! [`graph::test_support`](crate::agent::graph::test_support)).

    use std::collections::VecDeque;
    use std::sync::{Arc, Mutex};

    use claw_tool::{Tool, ToolGroup};

    use super::ControlSink;

    /// A fresh, empty control sink.
    pub(crate) fn sink() -> ControlSink {
        Arc::new(Mutex::new(VecDeque::new()))
    }

    /// The tool named `name` in `group` (cloned), panicking if absent.
    pub(crate) fn tool_named(group: &ToolGroup, name: &str) -> Tool {
        group
            .tools()
            .iter()
            .find(|tool| tool.name() == name)
            .unwrap()
            .clone()
    }
}
