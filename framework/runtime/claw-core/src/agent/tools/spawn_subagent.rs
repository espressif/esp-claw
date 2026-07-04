//! `spawn_subagent(kind, goal, termination)` — request a child agent of `kind`
//! to work on `goal`.

use std::sync::Arc;

use claw_capability::{
    tool_metadata, SyncToolHandler, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
    ToolSpec,
};
use claw_permission::{Action, RiskClass};

use crate::agent::graph::{AgentContext, SpawnPolicy, TerminationPolicy};
use crate::agent::kind::AgentKind;
use crate::agent::manifest::AgentManifest;

use super::string_argument;

/// Read one string argument and reject values that are empty after trimming.
/// Missing, non-string, and empty values are invalid arguments; whitespace-only
/// values are rejected after trim as a dynamic tool validation failure.
fn non_blank_argument(arguments_json: &str, key: &str) -> Result<String, ToolError> {
    let raw = string_argument(arguments_json, key)?;
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

/// Parse the `spawn_subagent` tool's `termination` argument; empty defaults to
/// [`AutoOnIdle`](TerminationPolicy::AutoOnIdle).
///
/// # Errors
///
/// [`ToolError::InvokeRejected`] for any value other than `auto` / `manual`.
fn parse_termination(raw: &str) -> Result<TerminationPolicy, ToolError> {
    match raw.trim() {
        "" | "auto" => Ok(TerminationPolicy::AutoOnIdle),
        "manual" => Ok(TerminationPolicy::Manual),
        other => Err(ToolError::InvokeRejected(format!(
            "spawn_subagent 'termination' must be one of auto|manual, got '{other}'"
        ))),
    }
}

/// Creates a child agent (scoped to the parent's `allowed_kinds`) via the graph
/// context.
pub(crate) struct SpawnSubagentTool {
    context: Arc<AgentContext>,
    /// The parent kind's `allowed_kinds`, enforced before any spawn is requested.
    /// The matching menu the model reads up front is served by the sibling
    /// `list_spawnable_agents` tool, which renders the same policy's catalog.
    policy: SpawnPolicy,
}

impl SpawnSubagentTool {
    /// Build the tool over the agent's `context` and its spawn `policy`.
    pub(crate) fn new(context: Arc<AgentContext>, policy: SpawnPolicy) -> Self {
        Self { context, policy }
    }
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
            tracing::warn!(requested_kind = %kind, "spawn kind rejected by allowed_kinds");
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
            tracing::warn!(requested_kind = %kind, "spawn of an unknown (non-baked) kind refused");
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
        let termination =
            parse_termination(&string_argument(call.arguments_json(), "termination")?)?;
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

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::agent::base_agent::AgentId;
    use crate::agent::graph::test_support::{context_for, spawned_kinds, RecordingHost};
    use crate::agent::graph::{GraphEffect, GraphHost};
    use claw_capability::RawToolInvocation;

    fn spawn_tool(host: Arc<RecordingHost>, policy: SpawnPolicy) -> SpawnSubagentTool {
        let context = context_for(host as Arc<dyn GraphHost>, AgentId(1));
        SpawnSubagentTool::new(context, policy)
    }

    fn call<'a>(arguments_json: &'a str) -> ToolInvocation<'a> {
        ToolInvocation::try_from(RawToolInvocation {
            id: Some("t1"),
            name: "spawn_subagent",
            arguments_json,
        })
        .unwrap()
    }

    #[test]
    fn termination_policy_parses_auto_manual_and_rejects_others() {
        assert_eq!(
            parse_termination("").unwrap(),
            TerminationPolicy::AutoOnIdle
        );
        assert_eq!(
            parse_termination("auto").unwrap(),
            TerminationPolicy::AutoOnIdle
        );
        assert_eq!(
            parse_termination("manual").unwrap(),
            TerminationPolicy::Manual
        );
        assert!(matches!(
            parse_termination("forever"),
            Err(ToolError::InvokeRejected(_))
        ));
    }

    #[test]
    fn spawn_rejects_disallowed_kind_without_spawning() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(
            Arc::clone(&host),
            SpawnPolicy::Only(vec![AgentKind::new("worker")]),
        );

        let output = tool
            .invoke(&call(r#"{"kind":"researcher","name":"r","goal":"x"}"#))
            .unwrap();

        // Refused as a matched tool error (not Err), and no spawn was emitted.
        assert!(!output.ok);
        assert!(output.output.contains("not permitted"));
        assert!(output.output.contains("worker"));
        assert!(host.effects.lock().unwrap().is_empty());
    }

    #[test]
    fn spawn_allows_permitted_kind() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(
            Arc::clone(&host),
            SpawnPolicy::Only(vec![AgentKind::new("worker")]),
        );

        let output = tool
            .invoke(&call(r#"{"kind":"worker","name":"w","goal":"x"}"#))
            .unwrap();

        assert!(output.ok);
        assert_eq!(spawned_kinds(&host), &[AgentKind::new("worker")]);
    }

    #[test]
    fn spawn_policy_any_allows_any_baked_kind() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(Arc::clone(&host), SpawnPolicy::Any);

        // `conversation` is a baked kind outside any `Only` allow-set, so it
        // exercises `Any` permitting kinds beyond a fixed list.
        let output = tool
            .invoke(&call(r#"{"kind":"conversation","name":"chat","goal":"x"}"#))
            .unwrap();

        assert!(output.ok);
        assert_eq!(spawned_kinds(&host), &[AgentKind::new("conversation")]);
    }

    #[test]
    fn spawn_carries_the_trimmed_name_into_the_effect() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(
            Arc::clone(&host),
            SpawnPolicy::Only(vec![AgentKind::new("worker")]),
        );

        let output = tool
            .invoke(&call(r#"{"kind":"worker","name":"  scout  ","goal":"x"}"#))
            .unwrap();

        assert!(output.ok);
        // The id-based confirmation also echoes the name for the model.
        assert!(output.output.contains("named 'scout'"));
        let effects = host.effects.lock().unwrap();
        let name = effects
            .iter()
            .find_map(|(_, effect)| match effect {
                GraphEffect::Spawn { name, .. } => Some(name.clone()),
                _ => None,
            })
            .unwrap();
        assert_eq!(name, Some("scout".to_string()));
    }

    #[test]
    fn spawn_requires_name_in_handler_validation() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(
            Arc::clone(&host),
            SpawnPolicy::Only(vec![AgentKind::new("worker")]),
        );

        let missing_name = tool.invoke(&call(r#"{"kind":"worker","goal":"x"}"#));
        let missing_name = missing_name.unwrap_err();
        assert!(matches!(missing_name.error, ToolError::InvalidArguments(_)));

        let empty_name = tool.invoke(&call(r#"{"kind":"worker","name":"","goal":"x"}"#));
        let empty_name = empty_name.unwrap_err();
        assert!(matches!(empty_name.error, ToolError::InvalidArguments(_)));

        assert!(host.effects.lock().unwrap().is_empty());
    }

    #[test]
    fn spawn_rejects_whitespace_only_name_after_trim() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(
            Arc::clone(&host),
            SpawnPolicy::Only(vec![AgentKind::new("worker")]),
        );

        // Schema `minLength` accepts whitespace; invoke trims and rejects blank.
        let error = tool
            .invoke(&call(r#"{"kind":"worker","name":"   ","goal":"x"}"#))
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvokeRejected(_)));
        assert!(host.effects.lock().unwrap().is_empty());
    }

    #[test]
    fn spawn_any_rejects_an_unknown_kind_without_spawning() {
        let host = Arc::new(RecordingHost::default());
        let tool = spawn_tool(Arc::clone(&host), SpawnPolicy::Any);

        // `Any` permits any string, but a non-baked kind cannot be built — it must
        // be refused here, not "spawned" and then silently dropped at materialize.
        let output = tool
            .invoke(&call(r#"{"kind":"ghost","name":"g","goal":"x"}"#))
            .unwrap();

        assert!(!output.ok);
        assert!(output.output.contains("not a known agent kind"));
        // The valid kinds are listed to steer the model.
        assert!(output.output.contains("worker"));
        // Nothing was emitted to the graph.
        assert!(host.effects.lock().unwrap().is_empty());
    }
}
