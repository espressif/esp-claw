//! Aggregating tools: named [`ToolGroup`]s, the per-agent [`ToolSet`] the
//! iteration loop consumes, and the [`AllowedTools`] phase allow-set.

use std::collections::{HashMap, HashSet};

use claw_permission::Action;
use serde_json::Value;
use thiserror::Error;

use jsonschema::Validator;

use crate::handler::{
    tool_invoke_err, Tool, ToolError, ToolInvocation, ToolInvokeError, ToolOutput,
};
use crate::validate::{compile_argument_validator, parse_arguments_json, validate_arguments};

/// A named bundle of [`Tool`]s registered together.
///
/// A group is the unit of registration — its `name` is a `&'static str` group
/// identity (e.g. `"fs"`, `"web"`) attached to every tool it carries, for
/// provenance and logging. Grouping does *not* namespace dispatch: tool names
/// stay flat and globally unique across groups (see [`ToolSet::from_groups`]).
pub struct ToolGroup {
    name: &'static str,
    tools: Vec<Tool>,
}

impl ToolGroup {
    /// Bundle `tools` under the group identity `name`.
    pub fn new(name: &'static str, tools: impl IntoIterator<Item = Tool>) -> Self {
        Self {
            name,
            tools: tools.into_iter().collect(),
        }
    }

    /// The group's identity.
    pub fn name(&self) -> &'static str {
        self.name
    }

    /// The tools in this group.
    pub fn tools(&self) -> &[Tool] {
        &self.tools
    }
}

/// Group label for tools registered without an explicit [`ToolGroup`].
pub const DEFAULT_TOOL_GROUP: &str = "default";

/// One tool plus its group identity, as stored for dispatch.
struct Entry {
    tool: Tool,
    /// The [`ToolGroup`] this tool was registered under (provenance).
    group: &'static str,
    /// Compiled validator for `function.parameters`, built at assembly time.
    argument_validator: Validator,
}

/// Error from assembling a [`ToolSet`].
#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum ToolSetError {
    /// Two tools (within or across groups) share a name; dispatch is by flat
    /// name, so this would silently shadow one — rejected at construction.
    #[error("duplicate tool name across groups: {0}")]
    DuplicateToolName(String),
    /// A tool's `function.parameters` JSON Schema could not be compiled.
    #[error("tool '{tool}' parameters JSON Schema is invalid: {details}")]
    InvalidParameterSchema { tool: String, details: String },
}

/// A set of [`Tool`]s, organized by group, ready for one or more iterations.
///
/// Built once from groups; precomputes the combined schemas JSON and a flat
/// name→tool dispatch map. Keys are owned `String`s copied from each tool's
/// [`name()`](crate::ToolHandler::name), and each entry carries its group label.
/// Dispatch is O(1) and flat across all groups; the group is metadata only.
/// This is the aggregate the iteration loop consumes directly, via
/// [`schemas_json`](Self::schemas_json) and [`invoke`](Self::invoke).
///
/// # Soft tools (phase gating)
///
/// The set is also the single owner of the "soft-hide" gating state: the full
/// schema in [`schemas_json`](Self::schemas_json) is always sent (so the cached
/// `tools` prefix never moves), while [`active`](Self::set_active_tools) — when
/// set — restricts which of those tools may actually *run* this phase. The two
/// prompt surfaces it produces stay together here:
/// - [`tool_context`](Self::tool_context): the **static** per-tool usage block
///   (belongs in the cached system prefix), and
/// - [`extra_tool_context`](Self::extra_tool_context): the **dynamic** phase note
///   naming the currently-active tools (belongs in the ephemeral request tail).
///
/// The runner consults [`is_allowed`](Self::is_allowed) before invoking; callers
/// place the two context strings. Soft tools are thus wholly a `claw-tool`
/// concern — the agent only decides *when* to flip the set and *where* the two
/// strings go.
///
/// # Wire surfaces are precomputed
///
/// All three strings handed to a request are **rendered once and cached**, so
/// per-request access is a free borrow (not a rebuild): [`schemas_json`](Self::schemas_json)
/// and [`tool_context`](Self::tool_context) are rebuilt only when the tool
/// membership changes, and [`extra_tool_context`](Self::extra_tool_context) only
/// when the active allow-set changes. The two static surfaces are emitted in tool
/// **name order**, so their bytes are stable across process restarts — the
/// server-side prompt cache keys on those bytes.
pub struct ToolSet {
    by_name: HashMap<String, Entry>,
    /// `[schema, schema, …]` serialized once in name order, or `None` when empty.
    schemas_json: Option<String>,
    /// The static per-tool usage block, rendered once in name order, or `None`
    /// when no tool carries usage. Rebuilt alongside `schemas_json`.
    tool_context: Option<String>,
    /// The soft-hide allow-set: which tools may *execute* this phase. `None` is
    /// ungated (every tool may run); `Some` restricts execution to its names
    /// without touching the schema sent to the model.
    active: Option<AllowedTools>,
    /// The dynamic phase note rendered from `active`, or `None` when ungated.
    /// Rebuilt whenever `active` changes.
    extra_context: Option<String>,
}

/// Dev-time consistency check for a tool's schema, run when a [`ToolSet`] is
/// assembled. Verifies the schema text is a JSON object **and** that its
/// `function.name` matches the handler's [`name()`](crate::ToolHandler::name) —
/// the one invariant the [`tool_metadata!`](crate::tool_metadata) macro cannot
/// enforce at compile time (the macro guarantees `name()` equals the schema
/// filename, but not the `name` field *inside* the JSON).
///
/// Every check is wrapped in `debug_assert*!`, so this is compiled out and costs
/// nothing in release builds; the JSON is parsed only under `cfg(debug_assertions)`.
fn debug_validate_schema(name: &str, schema: &str) {
    debug_assert!(
        serde_json::from_str::<Value>(schema).is_ok_and(|value| value.is_object()),
        "tool '{name}' returned an invalid JSON object schema: {schema}"
    );
    debug_assert_eq!(
        serde_json::from_str::<Value>(schema)
            .ok()
            .as_ref()
            .and_then(|value| value.get("function"))
            .and_then(|function| function.get("name"))
            .and_then(Value::as_str),
        Some(name),
        "tool '{name}' schema function.name must match handler name(): {schema}"
    );
}

/// Insert one `tool` under `group_name`, enforcing the flat-unique-name rule.
/// Shared by [`ToolSet::from_groups`] and [`ToolSet::extend_with_group`] so the
/// invariant (no name collision + a dev-time schema check) lives in one place.
///
/// # Errors
///
/// [`ToolSetError::DuplicateToolName`] when `tool`'s name already exists in
/// `by_name`; the map is left unchanged in that case.
fn insert_tool(
    by_name: &mut HashMap<String, Entry>,
    tool: Tool,
    group_name: &'static str,
) -> Result<(), ToolSetError> {
    let name = tool.name();
    if by_name.contains_key(name) {
        return Err(ToolSetError::DuplicateToolName(name.to_string()));
    }
    debug_validate_schema(name, tool.schema());
    let argument_validator = compile_argument_validator(name, tool.schema())?;
    by_name.insert(
        name.to_string(),
        Entry {
            tool,
            group: group_name,
            argument_validator,
        },
    );
    Ok(())
}

impl ToolSet {
    /// Assemble ungrouped tools under [`DEFAULT_TOOL_GROUP`].
    ///
    /// See [`from_groups`](Self::from_groups) for the grouped form and the
    /// duplicate-name rule.
    pub fn new(tools: impl IntoIterator<Item = Tool>) -> Result<Self, ToolSetError> {
        Self::from_groups([ToolGroup::new(DEFAULT_TOOL_GROUP, tools)])
    }

    /// Assemble a set from named groups.
    ///
    /// Tool names must be globally unique across all groups; a collision returns
    /// [`ToolSetError::DuplicateToolName`] rather than silently shadowing a tool.
    /// The combined schema array is serialized once here so
    /// [`schemas_json`](Self::schemas_json) is free per request.
    pub fn from_groups(groups: impl IntoIterator<Item = ToolGroup>) -> Result<Self, ToolSetError> {
        let mut by_name: HashMap<String, Entry> = HashMap::new();
        for group in groups {
            for tool in group.tools {
                insert_tool(&mut by_name, tool, group.name)?;
            }
        }
        Ok(Self::from_entries(by_name))
    }

    /// An empty set — no tools, no schemas. The infallible base other groups are
    /// merged onto with [`extend_with_group`](Self::extend_with_group).
    pub fn empty() -> Self {
        Self::from_entries(HashMap::new())
    }

    /// Build a set from a populated dispatch map and render its static caches.
    /// The single constructor [`from_groups`](Self::from_groups) and
    /// [`empty`](Self::empty) share, so field defaults live in one place.
    fn from_entries(by_name: HashMap<String, Entry>) -> Self {
        let mut set = Self {
            by_name,
            schemas_json: None,
            tool_context: None,
            active: None,
            extra_context: None,
        };
        set.rebuild_static_caches();
        set
    }

    /// Merge another [`ToolGroup`] into this set, re-checking the flat-unique-name
    /// rule against the tools already present.
    ///
    /// Used to fold an agent's built-in (internal) tool group onto the caller's
    /// tools after construction. The cached wire surfaces are rebuilt once here.
    ///
    /// # Errors
    ///
    /// [`ToolSetError::DuplicateToolName`] if a tool in `group` collides with one
    /// already in the set (or another in the same group); no tool is inserted past
    /// the collision and the existing caches are left intact.
    pub fn extend_with_group(&mut self, group: ToolGroup) -> Result<(), ToolSetError> {
        for tool in group.tools {
            insert_tool(&mut self.by_name, tool, group.name)?;
        }
        self.rebuild_static_caches();
        Ok(())
    }

    /// Re-render the membership-derived caches (`schemas_json` + `tool_context`)
    /// from the current entries, both in tool **name order** so the cached wire
    /// bytes are deterministic across builds (the backing map is unordered). One
    /// sort serves both surfaces.
    fn rebuild_static_caches(&mut self) {
        let mut entries: Vec<(&str, &Entry)> = self
            .by_name
            .iter()
            .map(|(name, entry)| (name.as_str(), entry))
            .collect();
        entries.sort_unstable_by_key(|(name, _)| *name);

        // Schema array: splice each tool's schema text, no re-serialization.
        self.schemas_json = (!entries.is_empty()).then(|| {
            let schemas: Vec<&str> = entries
                .iter()
                .map(|(_, entry)| entry.tool.schema())
                .collect();
            format!("[{}]", schemas.join(","))
        });

        // Usage block: stitch every tool's usage prose under one Markdown header.
        let mut usage_block = String::new();
        for (name, entry) in &entries {
            let Some(usage) = entry.tool.usage() else {
                continue;
            };
            usage_block.push_str(if usage_block.is_empty() {
                "# Tool usage\n\n## "
            } else {
                "\n\n## "
            });
            usage_block.push_str(name);
            usage_block.push('\n');
            usage_block.push_str(usage.trim());
        }
        self.tool_context = (!usage_block.is_empty()).then_some(usage_block);
    }

    /// Re-render the dynamic phase note from the current `active` allow-set, or
    /// clear it when ungated. Called whenever `active` changes.
    fn rebuild_active_context(&mut self) {
        self.extra_context = self.active.as_ref().map(|active| {
            let names = active.sorted_names();
            if names.is_empty() {
                "No tools are available in the current phase; do not call any tool.".to_string()
            } else {
                format!(
                    "Tools available in the current phase: {}. Other tools are \
                     temporarily unavailable — do not call them.",
                    names.join(", ")
                )
            }
        });
    }

    /// True when no tools were added.
    pub fn is_empty(&self) -> bool {
        self.by_name.is_empty()
    }

    /// The group a tool belongs to, or `None` if there is no such tool.
    pub fn group_of(&self, name: &str) -> Option<&'static str> {
        self.by_name.get(name).map(|entry| entry.group)
    }

    /// The combined OpenAI-style tools JSON array sent to the LLM, or `None` when
    /// there are no tools. Precomputed at construction, so this is a free borrow.
    pub fn schemas_json(&self) -> Option<&str> {
        self.schemas_json.as_deref()
    }

    /// The soft-tools prompt section: every tool's [`usage`](crate::ToolHandler::usage)
    /// prose, stitched into one Markdown block, or `None` when no tool carries
    /// usage. This is the *content* of the tool-policy prompt block; an assembler
    /// (`claw-context`) decides where it goes. The schema (the API surface) is the
    /// separate [`schemas_json`](Self::schemas_json) output.
    ///
    /// Rendered once in name order (see [`schemas_json`](Self::schemas_json)); this
    /// is a free borrow of the cache.
    pub fn tool_context(&self) -> Option<&str> {
        self.tool_context.as_deref()
    }

    // -- Soft tools (phase gating) ------------------------------------------

    /// Restrict execution to `allowed` for the current phase ("soft-hide").
    ///
    /// The schema sent to the model is unchanged; only [`is_allowed`](Self::is_allowed)
    /// (and the [`extra_tool_context`](Self::extra_tool_context) note) reflect the
    /// new set. Replaces any previous allow-set.
    pub fn set_active_tools(&mut self, allowed: AllowedTools) {
        self.active = Some(allowed);
        self.rebuild_active_context();
    }

    /// Drop phase gating: every tool in the set may run again (the default).
    pub fn clear_active_tools(&mut self) {
        self.active = None;
        self.rebuild_active_context();
    }

    /// Builder form of [`set_active_tools`](Self::set_active_tools).
    #[must_use]
    pub fn with_active_tools(mut self, allowed: AllowedTools) -> Self {
        self.set_active_tools(allowed);
        self
    }

    /// The current soft-hide allow-set, or `None` when ungated.
    pub fn active_tools(&self) -> Option<&AllowedTools> {
        self.active.as_ref()
    }

    /// Whether the tool named `name` may *execute* this phase. `true` when
    /// ungated (no allow-set) or when `name` is in the allow-set; the runner
    /// consults this before invoking.
    pub fn is_allowed(&self, name: &str) -> bool {
        self.active
            .as_ref()
            .is_none_or(|active| active.contains(name))
    }

    /// The **dynamic** soft-tools prompt note: a single line naming the tools
    /// permitted this phase, or `None` when ungated (no note).
    ///
    /// This is the volatile counterpart to [`tool_context`](Self::tool_context)
    /// (the static usage block): it changes whenever the active set changes, so
    /// it belongs in the **ephemeral request tail** (never the cached prefix, and
    /// never persisted). Its wording is kept here, beside enforcement, so the
    /// prose the model reads can never drift from what [`is_allowed`](Self::is_allowed)
    /// will actually permit. An empty allow-set yields a "no tools" note.
    ///
    /// Rendered once when the active set changes; this is a free borrow of the cache.
    pub fn extra_tool_context(&self) -> Option<&str> {
        self.extra_context.as_deref()
    }

    /// Classify `call` into a permission [`Action`] via its tool, or `None` when
    /// no tool owns `call.name` (an unknown call cannot be classified).
    pub fn classify(&self, call: &ToolInvocation<'_>) -> Option<Action> {
        self.by_name
            .get(call.name)
            .map(|entry| entry.tool.classify(call))
    }

    /// Whether the tool named `name` may run concurrently, or `None` when there is
    /// no such tool.
    pub fn concurrent(&self, name: &str) -> Option<bool> {
        self.by_name.get(name).map(|entry| entry.tool.concurrent())
    }

    /// Dispatch one model `tool_call` to its tool by name.
    ///
    /// Arguments are parsed and validated against the tool's parameter JSON Schema
    /// before [`ToolHandler::invoke`] runs. Returns [`ToolError::NotFound`] when no
    /// tool owns `call.name`; schema and JSON parse failures return the matching
    /// [`ToolError`] variant; dynamic rejections come from `invoke`.
    pub fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        // Note: the per-call `toolcall` span is created one layer up, in the
        // iteration loop, so it also covers calls refused by soft-hide gating
        // (which never reach here).
        match self.by_name.get(call.name) {
            Some(entry) => {
                let arguments = parse_arguments_json(call.arguments_json)?;
                validate_arguments(&entry.argument_validator, &arguments)?;
                entry.tool.invoke(call)
            }
            None => Err(tool_invoke_err(ToolError::NotFound(call.name.to_string()))),
        }
    }
}

/// The set of tool names allowed to *execute* in the current phase ("soft-hide"
/// gating) — the input vocabulary handed to
/// [`ToolSet::set_active_tools`](ToolSet::set_active_tools).
///
/// Soft-hide keeps the full [`ToolSet`] schema (the superset) in the prompt so
/// the cached `tools` prefix never changes, while restricting which of those
/// tools may actually run right now. Once installed on a [`ToolSet`], the runner
/// consults [`ToolSet::is_allowed`](ToolSet::is_allowed) before invoking each
/// tool and refuses any call whose name is absent (the model is handed a tool
/// error instead). A set with no allow-set installed is "ungated": every tool
/// may run.
///
/// Names are owned `String`s so both baked tools (compile-time names) and
/// runtime-registered tools (dynamic names) can be gated uniformly.
///
/// # Examples
///
/// ```
/// use claw_tool::AllowedTools;
///
/// let allowed = AllowedTools::new(["read_file", "list_dir"]);
/// assert!(allowed.contains("read_file"));
/// assert!(!allowed.contains("write_file"));
/// ```
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AllowedTools {
    names: HashSet<String>,
}

impl AllowedTools {
    /// Build an allow-set from a collection of permitted tool names.
    pub fn new(names: impl IntoIterator<Item = impl Into<String>>) -> Self {
        Self {
            names: names.into_iter().map(Into::into).collect(),
        }
    }

    /// True when `name` is permitted to execute this phase.
    pub fn contains(&self, name: &str) -> bool {
        self.names.contains(name)
    }

    /// True when no tool is permitted (an empty allow-set blocks everything).
    pub fn is_empty(&self) -> bool {
        self.names.is_empty()
    }

    /// The permitted names in a stable (alphabetical) order.
    ///
    /// The backing set is unordered; sorting gives deterministic output for the
    /// phase note built from this allow-set (and for tests).
    pub fn sorted_names(&self) -> Vec<&str> {
        let mut names: Vec<&str> = self.names.iter().map(String::as_str).collect();
        names.sort_unstable();
        names
    }
}

impl<'a> FromIterator<&'a str> for AllowedTools {
    fn from_iter<T: IntoIterator<Item = &'a str>>(iter: T) -> Self {
        Self {
            names: iter.into_iter().map(str::to_string).collect(),
        }
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::handler::{ToolHandler, ToolInvokeError};

    /// A handler whose `name()` deliberately disagrees with the `function.name`
    /// inside its schema — the inconsistency `tool_metadata!` cannot catch.
    struct MismatchedNameTool;

    impl ToolHandler for MismatchedNameTool {
        fn name(&self) -> &str {
            "alpha"
        }

        fn schema(&self) -> &str {
            r#"{"type":"function","function":{"name":"beta","parameters":{"type":"object"}}}"#
        }

        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: String::new(),
                ok: true,
            })
        }
    }

    /// In dev builds, assembling a set surfaces a `name()` vs schema `function.name`
    /// mismatch via the `debug_assert_eq!` in `debug_validate_schema`. Gated to
    /// `debug_assertions` because that check is compiled out of release builds.
    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "function.name must match handler name")]
    fn schema_name_mismatch_is_caught_in_dev() {
        let _ = ToolSet::new([Tool::new(MismatchedNameTool)]);
    }

    /// A tool carrying soft-tools usage prose, for the `tool_context` tests.
    struct UsageTool {
        name: String,
        schema: String,
        usage: Option<String>,
    }

    impl UsageTool {
        fn new(name: &str, usage: Option<&str>) -> Self {
            Self {
                schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
                name: name.to_string(),
                usage: usage.map(str::to_string),
            }
        }
    }

    impl ToolHandler for UsageTool {
        fn name(&self) -> &str {
            &self.name
        }
        fn schema(&self) -> &str {
            &self.schema
        }
        fn usage(&self) -> Option<&str> {
            self.usage.as_deref()
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: String::new(),
                ok: true,
            })
        }
    }

    #[test]
    fn tool_context_is_none_when_no_tool_has_usage() {
        let set = ToolSet::new([Tool::new(UsageTool::new("bare", None))]).unwrap();
        assert_eq!(set.tool_context(), None);
    }

    #[test]
    fn tool_context_stitches_usage_in_name_order() {
        let set = ToolSet::new([
            Tool::new(UsageTool::new("zeta", Some("Zeta does Z."))),
            Tool::new(UsageTool::new("alpha", Some("Alpha does A."))),
            Tool::new(UsageTool::new("silent", None)),
        ])
        .unwrap();

        assert_eq!(
            set.tool_context(),
            Some("# Tool usage\n\n## alpha\nAlpha does A.\n\n## zeta\nZeta does Z.")
        );
    }

    #[test]
    fn ungated_set_allows_every_tool_and_has_no_extra_context() {
        let set = ToolSet::new([Tool::new(UsageTool::new("read", None))]).unwrap();
        assert!(set.is_allowed("read"));
        assert!(set.is_allowed("anything")); // ungated: even unknown names pass gating
        assert_eq!(set.active_tools(), None);
        assert_eq!(set.extra_tool_context(), None);
    }

    #[test]
    fn active_set_gates_execution_and_renders_a_phase_note() {
        let mut set = ToolSet::new([
            Tool::new(UsageTool::new("read", None)),
            Tool::new(UsageTool::new("write", None)),
        ])
        .unwrap();
        set.set_active_tools(AllowedTools::new(["read"]));

        assert!(set.is_allowed("read"));
        assert!(!set.is_allowed("write"));

        let note = set.extra_tool_context().unwrap();
        assert!(note.contains("Tools available in the current phase"));
        assert!(note.contains("read"));
        assert!(!note.contains("write"));
    }

    #[test]
    fn empty_active_set_blocks_everything_with_a_no_tools_note() {
        let mut set = ToolSet::new([Tool::new(UsageTool::new("read", None))]).unwrap();
        set.set_active_tools(AllowedTools::default());

        assert!(!set.is_allowed("read"));
        assert_eq!(
            set.extra_tool_context(),
            Some("No tools are available in the current phase; do not call any tool.")
        );
    }

    #[test]
    fn clearing_active_tools_restores_ungated() {
        let mut set = ToolSet::new([Tool::new(UsageTool::new("read", None))]).unwrap();
        set.set_active_tools(AllowedTools::new(["other"]));
        assert!(!set.is_allowed("read"));

        set.clear_active_tools();
        assert!(set.is_allowed("read"));
        assert_eq!(set.extra_tool_context(), None);
    }

    #[test]
    fn invoke_rejects_arguments_that_fail_schema_validation() {
        const SCHEMA: &str = r#"{
            "type": "function",
            "function": {
                "name": "needs_name",
                "parameters": {
                    "type": "object",
                    "properties": { "name": { "type": "string" } },
                    "required": ["name"]
                }
            }
        }"#;
        struct NeedsNameTool;
        impl ToolHandler for NeedsNameTool {
            fn name(&self) -> &str {
                "needs_name"
            }
            fn schema(&self) -> &str {
                SCHEMA
            }
            fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
                Ok(ToolOutput {
                    output: "ok".into(),
                    ok: true,
                })
            }
        }
        let set = ToolSet::new([Tool::new(NeedsNameTool)]).unwrap();
        let error = set
            .invoke(&ToolInvocation {
                id: Some("t1"),
                name: "needs_name",
                arguments_json: "{}",
            })
            .unwrap_err();
        assert!(matches!(error.error, ToolError::InvalidArguments(_)));
        assert!(error.retries.is_none());
    }
}
