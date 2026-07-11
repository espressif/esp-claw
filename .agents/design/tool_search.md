# Tool Search

```rust
type ToolGroupId = String

struct ToolGroup {
    id: ToolGroupId
    description: String
}

struct ToolMeta {
    group_id: ToolGroupId
    description: String
    deferred: bool
}

struct ToolRegistryEntry {
    tool: Tool
    enabled: bool
    meta: ToolMeta
}

struct ToolRegistry {
    tools: HashMap<ToolName, ToolRegistryEntry>,
    groups: HashMap<ToolGroupId, ToolGroup>,

    pub fn register(tool: Tool) -> Result<()> // uses default group/deferred metadata
    pub fn register_with_meta(tool: Tool, meta: ToolMeta) -> Result<()>
    pub fn search(query: &str, limit: usize) -> Vec<ToolSearchGroup>
    pub fn tools_in_group(group_id: &str) -> Vec<ToolName>
}

struct ToolSearchGroup {
    group_id: ToolGroupId
    description: String
    tools: Vec<ToolSearchTool>
}

struct ToolSearchTool {
    name: ToolName
    description: String
}

struct ToolSet {
    pub fn temporarily_enable_tool(name: ToolName) -> Result<()> // existing API
    pub fn clear_temporary_tools() // existing API
}
```

## Goal

Avoid sending every tool schema at session start.

Keep only core tools visible by default:

```text
tool_search
tool_load
skill_list
skill_activate
conversation_end
```

All other tools stay registered, started, and searchable, but their schemas are not visible until loaded.

## Minimal Model

Do not add a new runtime visibility system.

Use the existing `ToolSet` states:

```rust
Enabled // visible by default
Disabled // registered but hidden
TemporarilyEnabled // loaded by tool_load
TemporarilyDisabled // existing soft block
```

Deferred tools are inserted into each `ToolSet` as `Disabled`.

`tool_load` calls `temporarily_enable_tool()` for every tool in the selected group. The existing `schemas_json()`, `tool_context()`, `extra_tool_context()`, `classify()`, and `invoke()` paths already know how to handle `TemporarilyEnabled`.

## Flow

```text
model:
  tool_search({"query":"schedule reminder"})

tool_search:
  returns group ids, group descriptions, tool names, and short tool descriptions
  does not return schemas

model:
  tool_load({"group_id":"cap_scheduler"})

tool_load:
  finds tools in cap_scheduler
  temporarily_enable_tool(tool) for each one
  returns "Loaded cap_scheduler: scheduler_add, scheduler_list"

next model step:
  schemas for those tools are visible
  model calls scheduler_add or scheduler_list

cleanup:
  clear_temporary_tools()
```

## Tool Search Output

```json
{
  "groups": [
    {
      "group_id": "cap_scheduler",
      "description": "Create and inspect scheduled tasks.",
      "tools": [
        {
          "name": "scheduler_add",
          "description": "Create one scheduled task."
        },
        {
          "name": "scheduler_list",
          "description": "List scheduled tasks."
        }
      ]
    }
  ]
}
```

This is the whole search payload. No parameters, no examples, no JSON schema.

## Tool Load

```json
{"group_id":"cap_scheduler"}
```

MVP loads one group at a time.

No scope option. No pinned option. No unload tool. No persistence.

If a group is too large, `tool_load` rejects it and asks the model to load a narrower tool name later. Per-tool loading can be added only when a real group becomes too large.

## Tool Group Source

For native Rust tools:

```rust
registry.register_with_meta(
    Tool::from_sync(MyTool),
    ToolMeta {
        group_id: "core".into(),
        description: "Short one-line tool description.".into(),
        deferred: false,
    },
)
```

For C capabilities, use the existing capability group id:

```text
cap_lua
cap_scheduler
cap_router_mgr
cap_web_search
```

The current Rust C ABI receives `ClawCapDescriptor` without `group_id`. To avoid inventing a mapping table, add the smallest ABI bridge needed during capability registration:

```rust
claw_cap_get_descriptor_state(id_or_name) -> { group_id, state, active_calls }
```

Then `register_capability_tools()` registers each `CapTool` with:

```rust
ToolMeta {
    group_id: descriptor_info.group_id,
    description: descriptor.description,
    deferred: !is_default_visible_group(group_id),
}
```

## Default Visibility

```rust
fn default_tool_state(meta: &ToolMeta) -> ToolState {
    if meta.deferred {
        ToolState::Disabled
    } else {
        ToolState::Enabled
    }
}
```

Default-visible groups are only the small boot surface:

```text
core
skill
tool_search
```

Everything else is searchable but disabled in the `ToolSet` projection until `tool_load`.

## Persistence

Do not persist loaded tool state.

Existing `ToolSetState` is durable, so `TemporarilyEnabled` must not be exported as durable loaded state. Before export or during restore, normalize temporary states:

```rust
TemporarilyEnabled -> Disabled
TemporarilyDisabled -> Enabled
```

Persist only a short reminder if needed:

```text
Previously used tool groups: cap_scheduler, cap_router_mgr. Use tool_load if needed.
```

This reminder is text context, not tool visibility.

## Rules

- `tool_search` reads registry metadata only.
- `tool_search` never returns schemas.
- `tool_search` never changes tool visibility.
- `tool_load` uses existing `temporarily_enable_tool()`.
- `tool_load` does not persist.
- `tool_load` has no scope argument.
- Hidden schemas are not security. `classify()` and `invoke()` still require the tool to be enabled or temporarily enabled.
- Group loading is the MVP. Per-tool loading is a later pressure valve, not part of the first design.
