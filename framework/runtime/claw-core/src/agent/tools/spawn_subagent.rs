//! `spawn_subagent(kind, goal, termination)` — request a child agent of `kind`
//! to work on `goal`.

use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use claw_tool::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};

use crate::agent::graph::{AgentContext, SpawnPolicy, TerminationPolicy};
use crate::agent::kind::AgentKind;
use crate::agent::manifest::AgentManifest;

use super::optional_string_argument;

/// Read one string argument and reject values that are empty after trimming.
/// Missing, non-string, and empty values are invalid arguments; whitespace-only
/// values are rejected after trim as a dynamic tool validation failure.
fn non_blank_argument(arguments_json: &str, key: &str) -> Result<String, ToolError> {
    let Some(raw) = optional_string_argument(arguments_json, key)? else {
        return Err(ToolError::InvalidArguments(format!(
            "spawn_subagent '{key}' is required"
        )));
    };
    if raw.is_empty() {
        return Err(ToolError::InvalidArguments(format!(
            "spawn_subagent '{key}' is required"
        )));
    }
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Err(ToolError::InvokeRejected(format!(
            "spawn_subagent '{key}' must not be blank"
        )));
    }
    Ok(trimmed.to_string())
}

/// Creates a child agent (scoped to the parent's `allowed_kinds`) via the graph
/// context.
pub(crate) struct SpawnSubagentTool {
    pub(super) context: Arc<AgentContext>,
    /// The parent kind's `allowed_kinds`, enforced before any spawn is requested.
    /// The matching menu the model reads up front is served by the sibling
    /// `list_spawnable_agents` tool, which renders the same policy's catalog.
    pub(super) policy: SpawnPolicy,
}

impl ToolSpec for SpawnSubagentTool {
    tool_metadata!("spawn_subagent");

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        // Creating a child mutates the graph — worth a policy look, but reversible.
        Action::new("spawn_subagent", RiskClass::Moderate)
    }
}

impl SyncToolHandler for SpawnSubagentTool {
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let kind = AgentKind::new(non_blank_argument(call.arguments_json(), "kind")?);

        // Enforce the manifest's `allowed_kinds`. A disallowed kind is refused with
        // a matched tool error (`ok = false`, like soft-hide gating) so the model
        // can pick a permitted kind or do the work itself — not as `Err`, which
        // would fail the whole iteration and rob the model of self-correction.
        if !self.policy.allows(&kind) {
            tracing::warn!(name: "spawn_kind_rejected", kind = %kind.as_str());
            return Ok(ToolOutput {
                output: format!(
                    "spawn_subagent: kind '{kind}' is not permitted for this agent. \
                     Allowed: {}. This is a policy restriction, not a transient error: \
                     pick a permitted kind or handle the work yourself.",
                    self.policy.describe()
                ),
                ok: false,
            });
        }

        // The policy may permit the kind (notably `Any`, which permits any
        // *string*), but only a kind with a baked manifest can actually be built.
        // Without this guard a bogus kind would be reported as "spawned" here and
        // then silently dropped at `materialize_spawn`. Refuse it as a matched tool
        // error (like a disallowed kind) so the model can pick a real one.
        if AgentManifest::for_kind(kind.as_str()).is_none() {
            tracing::warn!(name: "spawn_unknown_kind_rejected", kind = %kind.as_str());
            let available = self
                .policy
                .catalog()
                .iter()
                .map(|(agent_kind, _)| agent_kind.as_str())
                .collect::<Vec<_>>()
                .join(", ");
            let available = if available.is_empty() {
                "(none)".to_string()
            } else {
                available
            };
            return Ok(ToolOutput {
                output: format!(
                    "spawn_subagent: '{kind}' is not a known agent kind, so it cannot be \
                     created. Spawnable kinds: {available}. Call list_spawnable_agents to see \
                     what you can spawn."
                ),
                ok: false,
            });
        }

        let name = non_blank_argument(call.arguments_json(), "name")?;
        let goal = non_blank_argument(call.arguments_json(), "goal")?;
        // Optional lifecycle policy: default one-shot (`auto`); `manual` keeps the
        // child alive and idle after it yields so this agent can supervise it.
        let termination = match optional_string_argument(call.arguments_json(), "termination")?
            .as_deref()
            .map(str::trim)
        {
            None | Some("") => TerminationPolicy::AutoOnIdle,
            Some(value) => TerminationPolicy::try_from(value).map_err(|_| {
                ToolError::InvokeRejected(format!(
                    "spawn_subagent 'termination' must be one of auto|manual, got '{value}'"
                ))
            })?,
        };
        let child = self
            .context
            .spawn(kind, Some(name.clone()), goal, termination);
        Ok(ToolOutput {
            output: format!(
                "Subagent {child} named '{name}' requested; its result will be reported back when it finishes."
            ),
            ok: true,
        })
    }
}
