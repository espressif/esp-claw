//! A global pool of [`Tool`]s — both baked (compile-time) and runtime-registered
//! — that [`ToolSet`](crate::ToolSet)s are assembled from.
//!
//! The registry owns every tool the system knows about. An agent's [`ToolSet`] is
//! a *selection* from this pool: `select(names)` picks a subset and builds the
//! set's combined schema + dispatch map. Because the set's keys are now owned
//! `String`s, both baked tools (`&'static str` names) and runtime tools (dynamic
//! names) are stored and selected uniformly.
//!
//! Typical lifetime:
//!
//! ```ignore
//! let mut registry = ToolRegistry::new();
//! registry.register(Tool::new(ReadFileTool));
//! registry.register(Tool::new(WriteFileTool));
//! // ...later, build an agent's tool set from its manifest:
//! let tool_set = registry.select(&["read_file", "write_file"])?;
//! ```

use std::collections::HashMap;

use crate::handler::Tool;
use crate::set::{ToolGroup, ToolSet, ToolSetError};

/// A pool of registered [`Tool`]s, keyed by name.
///
/// Tools are added with [`register`](Self::register) (keyed by the tool's own
/// [`name`](crate::ToolHandler::name)) or [`register_as`](Self::register_as)
/// (explicit key). A [`ToolSet`] is built from a subset via
/// [`select`](Self::select).
#[derive(Default)]
pub struct ToolRegistry {
    tools: HashMap<String, Tool>,
}

impl ToolRegistry {
    /// An empty registry.
    pub fn new() -> Self {
        Self::default()
    }

    /// Register `tool` under its own [`name`](crate::ToolHandler::name).
    /// Replaces any tool already registered under that name.
    pub fn register(&mut self, tool: Tool) {
        self.tools.insert(tool.name().to_string(), tool);
    }

    /// Register `tool` under an explicit `name` (when the manifest name differs
    /// from the tool's own function name). Replaces any tool already registered
    /// under that name.
    pub fn register_as(&mut self, name: impl Into<String>, tool: Tool) {
        self.tools.insert(name.into(), tool);
    }

    /// Remove the tool registered under `name`, if any. Returns the removed tool.
    pub fn unregister(&mut self, name: &str) -> Option<Tool> {
        self.tools.remove(name)
    }

    /// Look up one tool by name.
    pub fn get(&self, name: &str) -> Option<&Tool> {
        self.tools.get(name)
    }

    /// True when the registry contains a tool under `name`.
    pub fn contains(&self, name: &str) -> bool {
        self.tools.contains_key(name)
    }

    /// The number of registered tools.
    pub fn len(&self) -> usize {
        self.tools.len()
    }

    /// True when no tools are registered.
    pub fn is_empty(&self) -> bool {
        self.tools.is_empty()
    }

    /// Build a [`ToolSet`] from the named tools, placed under
    /// [`DEFAULT_TOOL_GROUP`].
    ///
    /// # Errors
    ///
    /// Returns `Err` wrapping the first name that is not in the registry (as a
    /// [`ToolSetError::DuplicateToolName`] would never fire for distinct names,
    /// this is the only failure mode). Also propagates any [`ToolSetError`] from
    /// the underlying set assembly (e.g. a name collision — shouldn't happen when
    /// the input names are distinct, but forwarded for safety).
    pub fn select(&self, names: &[&str]) -> Result<ToolSet, ToolRegistryError> {
        let tools: Vec<Tool> = names
            .iter()
            .map(|name| {
                self.tools
                    .get(*name)
                    .cloned()
                    .ok_or_else(|| ToolRegistryError::NotFound((*name).to_string()))
            })
            .collect::<Result<_, _>>()?;
        ToolSet::new(tools).map_err(ToolRegistryError::Set)
    }

    /// Build a [`ToolSet`] containing *every* tool in the registry, under
    /// [`DEFAULT_TOOL_GROUP`].
    ///
    /// # Errors
    ///
    /// Propagates any [`ToolSetError`] from the underlying set assembly.
    pub fn select_all(&self) -> Result<ToolSet, ToolRegistryError> {
        let tools: Vec<Tool> = self.tools.values().cloned().collect();
        ToolSet::new(tools).map_err(ToolRegistryError::Set)
    }

    /// Build a [`ToolGroup`] from the named tools with the given group label.
    ///
    /// # Errors
    ///
    /// [`ToolRegistryError::NotFound`] for the first name not in the registry.
    pub fn group(
        &self,
        group_name: &'static str,
        names: &[&str],
    ) -> Result<ToolGroup, ToolRegistryError> {
        let tools: Vec<Tool> = names
            .iter()
            .map(|name| {
                self.tools
                    .get(*name)
                    .cloned()
                    .ok_or_else(|| ToolRegistryError::NotFound((*name).to_string()))
            })
            .collect::<Result<_, _>>()?;
        Ok(ToolGroup::new(group_name, tools))
    }

    /// Iterate over all registered (name, tool) pairs.
    pub fn iter(&self) -> impl Iterator<Item = (&str, &Tool)> {
        self.tools.iter().map(|(name, tool)| (name.as_str(), tool))
    }
}

/// Error from a [`ToolRegistry`] operation.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum ToolRegistryError {
    /// A requested tool name is not in the registry.
    #[error("tool not found in registry: {0}")]
    NotFound(String),
    /// Forwarded from [`ToolSet`] assembly (e.g. a duplicate name).
    #[error(transparent)]
    Set(ToolSetError),
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::handler::{ToolHandler, ToolInvocation, ToolInvokeError, ToolOutput};

    struct DummyTool {
        tool_name: String,
        tool_schema: String,
    }

    impl DummyTool {
        fn new(name: &str) -> Self {
            Self {
                tool_schema: format!(r#"{{"type":"function","function":{{"name":"{name}"}}}}"#),
                tool_name: name.to_string(),
            }
        }
    }

    impl ToolHandler for DummyTool {
        fn name(&self) -> &str {
            &self.tool_name
        }
        fn schema(&self) -> &str {
            &self.tool_schema
        }
        fn invoke(&self, _call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
            Ok(ToolOutput {
                output: format!("ran:{}", self.tool_name),
                ok: true,
            })
        }
    }

    #[test]
    fn register_and_select() {
        let mut registry = ToolRegistry::new();
        registry.register(Tool::new(DummyTool::new("alpha")));
        registry.register(Tool::new(DummyTool::new("beta")));

        assert_eq!(registry.len(), 2);
        assert!(registry.contains("alpha"));
        assert!(!registry.contains("gamma"));

        let set = registry.select(&["alpha"]).unwrap();
        assert!(!set.is_empty());
        assert!(set.schemas_json().is_some());
    }

    #[test]
    fn select_unknown_name_errors() {
        let registry = ToolRegistry::new();
        let result = registry.select(&["missing"]);
        assert!(matches!(result, Err(ToolRegistryError::NotFound(name)) if name == "missing"));
    }

    #[test]
    fn select_all_includes_every_tool() {
        let mut registry = ToolRegistry::new();
        registry.register(Tool::new(DummyTool::new("a")));
        registry.register(Tool::new(DummyTool::new("b")));

        let set = registry.select_all().unwrap();
        assert!(set.schemas_json().unwrap().contains("\"a\""));
        assert!(set.schemas_json().unwrap().contains("\"b\""));
    }

    #[test]
    fn unregister_removes_tool() {
        let mut registry = ToolRegistry::new();
        registry.register(Tool::new(DummyTool::new("temp")));
        assert!(registry.contains("temp"));

        let removed = registry.unregister("temp");
        assert!(removed.is_some());
        assert!(!registry.contains("temp"));
    }

    #[test]
    fn register_as_uses_explicit_key() {
        let mut registry = ToolRegistry::new();
        registry.register_as("alias", Tool::new(DummyTool::new("real_name")));

        assert!(registry.contains("alias"));
        assert!(!registry.contains("real_name"));
    }
}
