# Adapter

```rust
// Agent only sees channels and tool groups.
```

## Agent Surface

```rust
enum AgentAdapter {
    Channel(ChannelAdapter),
    ToolGroup(ToolGroupAdapter),
}

struct ChannelAdapter {
    name: ChannelName,
    source_group: CapabilityGroupId,
    inbound: Option<CapabilityName>, // gateway/event source
    send_text: Option<CapabilityName>,
    send_image: Option<CapabilityName>,
    send_file: Option<CapabilityName>,
}

struct ToolGroupAdapter {
    group_id: CapabilityGroupId,
    source_group: CapabilityGroupId,
    tools: Vec<CapabilityName>,
    llm_default: bool,
}
```

## C Capability Registry

```rust
// Rust owns this registry and exposes it to C as claw_capability_registry_t.
struct CCapabilityRegistry {
    groups: HashMap<CapabilityGroupId, CCapabilityGroup>,

    pub fn register(capability: CCapability) -> Result<()> // single capability
    pub fn register_group(group: CCapabilityGroup) -> Result<()> // C component bundle
    pub fn start_all() -> Result<()> // lifecycle driver
    pub fn stop_all() -> Result<()> // reverse lifecycle driver
    pub fn invoke(name: &str, arguments_json: &str) -> Result<ToolOutput> // sync C tool call
    pub fn adapters() -> Vec<AgentAdapter> // projection, not ownership transfer
}

struct CCapabilityGroup {
    id: CapabilityGroupId,
    members: Vec<CCapability>,
    lifecycle: Lifecycle,
}

struct CCapability {
    id: CapabilityName,
    description: Option<String>,
    role: CCapabilityRole,
    lifecycle: Lifecycle,
}

enum CCapabilityRole {
    None,    // lifecycle-only service
    Tool,    // callable C capability
    Channel, // native C channel role
}
```

```rust
// Not agent adapter types.
Lifecycle
DirectCall
OutboundBackend
EventSource
```

```rust
fn adapt_group(group: CCapabilityGroup) -> Option<AgentAdapter> {
    if is_im_group(group.id) {
        return Some(AgentAdapter::Channel(adapt_im_group(group)))
    }

    if is_agent_tool_group(group.id) {
        return Some(AgentAdapter::ToolGroup(adapt_tool_group(group)))
    }

    None
}
```

## Rules

```rust
// CCapabilityRegistry is the authority.
C capability registration -> CCapabilityRegistry
CCapabilityRegistry -> AgentAdapter projection
CCapabilityRegistry -> invoke/lifecycle runtime

// Channel groups own both inbound and outbound platform plumbing.
gateway -> ChannelAdapter.inbound
send_message -> ChannelAdapter.send_text
send_image -> ChannelAdapter.send_image
send_file -> ChannelAdapter.send_file

// Tool groups are the only source for ToolSet.
ToolGroupAdapter.tools -> ToolSet

// Registry-only capabilities do not enter the agent surface.
Lifecycle service -> CCapabilityRegistry::start_all / CCapabilityRegistry::stop_all
Direct command -> CCapabilityRegistry::invoke by system/router/console

// Scheduler is a tool group from the agent point of view.
cap_scheduler -> ToolGroupAdapter
scheduler timer events -> EventRouter, not AgentAdapter
```

## Runtime Flow

```rust
// Inbound channel message.
ChannelAdapter.inbound -> AgentSystem::push_message -> Orchestrator::submit

// Model tool call.
GenericAgent -> ToolRunner -> ToolSetHandle::invoke -> CCapabilityRegistry::invoke

// Agent reply.
Orchestrator output -> AgentSystem -> ChannelAdapter -> CCapabilityRegistry::invoke(send_cap)

// Router bypass.
EventRouter::CallCap -> CCapabilityRegistry::invoke
EventRouter::RunAgent -> AgentSystem::push_message
EventRouter::SendMessage -> ChannelAdapter.send_text
```

## Channels

```rust
ChannelAdapter {
    name: "qq",
    source_group: "cap_im_qq",
    inbound: Some("qq_gateway"),
    send_text: Some("qq_send_message"),
    send_image: Some("qq_send_image"),
    send_file: Some("qq_send_file"),
}

ChannelAdapter {
    name: "feishu",
    source_group: "cap_im_feishu",
    inbound: Some("feishu_gateway"),
    send_text: Some("feishu_send_message"),
    send_image: Some("feishu_send_image"),
    send_file: Some("feishu_send_file"),
}

ChannelAdapter {
    name: "telegram",
    source_group: "cap_im_tg",
    inbound: Some("tg_gateway"),
    send_text: Some("tg_send_message"),
    send_image: Some("tg_send_image"),
    send_file: Some("tg_send_file"),
}

ChannelAdapter {
    name: "wechat",
    source_group: "cap_im_wechat",
    inbound: Some("wechat_gateway"),
    send_text: Some("wechat_send_message"),
    send_image: Some("wechat_send_image"),
    send_file: None,
}

ChannelAdapter {
    name: "web",
    source_group: "cap_im_local",
    inbound: Some("local_gateway"),
    send_text: Some("local_send_message"),
    send_image: None,
    send_file: None,
}
```

## Tool Groups

```rust
ToolGroupAdapter {
    group_id: "cap_files",
    source_group: "cap_files",
    llm_default: true,
    tools: [
        "read_file",
        "write_file",
        "delete_file",
        "copy_file",
        "move_file",
        "list_dir",
    ],
}

ToolGroupAdapter {
    group_id: "cap_scheduler",
    source_group: "cap_scheduler",
    llm_default: true,
    tools: [
        "scheduler_list",
        "scheduler_get",
        "scheduler_add",
        "scheduler_update",
        "scheduler_enable",
        "scheduler_disable",
        "scheduler_remove",
        "scheduler_pause",
        "scheduler_resume",
        "scheduler_trigger_now",
        "scheduler_reload",
    ],
}

ToolGroupAdapter {
    group_id: "cap_lua",
    source_group: "cap_lua",
    llm_default: true,
    tools: [
        "lua_run_script",
        "lua_run_script_async",
        "lua_list_async_jobs",
        "lua_get_async_job",
        "lua_tail_async_job",
        "lua_stop_async_job",
        "lua_stop_all_async_jobs",
    ],
}

ToolGroupAdapter {
    group_id: "cap_mcp_client",
    source_group: "cap_mcp_client",
    llm_default: false,
    tools: [
        "mcp_list_tools",
        "mcp_call_tool",
        "mcp_discover",
    ],
}

ToolGroupAdapter {
    group_id: "cap_system",
    source_group: "cap_system",
    llm_default: false,
    tools: [
        "get_system_info",
        "get_current_time",
        "restart_device",
    ],
}

ToolGroupAdapter {
    group_id: "cap_llm_inspect",
    source_group: "cap_llm_inspect",
    llm_default: true,
    tools: [
        "inspect_image",
    ],
}

ToolGroupAdapter {
    group_id: "cap_http_request",
    source_group: "cap_http_request",
    llm_default: true,
    tools: [
        "http_request",
    ],
}

ToolGroupAdapter {
    group_id: "cap_web_search",
    source_group: "cap_web_search",
    llm_default: true,
    tools: [
        "web_search",
    ],
}

ToolGroupAdapter {
    group_id: "cap_router_mgr",
    source_group: "cap_router_mgr",
    llm_default: true,
    tools: [
        "list_router_rules",
        "get_router_rule",
        "add_router_rule",
        "update_router_rule",
        "delete_router_rule",
        "reload_router_rules",
    ],
}
```

## Registry Only

```rust
// Not adapted into AgentAdapter.
CapabilityGroup("cap_mcp_server") {
    role: Lifecycle,
    service: "mcp_server",
}

CapabilityGroup("cap_llm_config") {
    role: DirectCall,
    tool: "llm_config_command",
}

CapabilityGroup("cap_cli") {
    role: DirectCall,
    tool: "CAP_CLI_NAME",
}

CapabilityGroup("cap_boards") {
    role: ResourceOnly,
}
```

## Final Rules

```rust
// There are exactly two agent adapter outputs.
ChannelAdapter
ToolGroupAdapter

// Platform send capabilities belong inside ChannelAdapter.
send_* != ToolGroup

// Scheduler belongs to ToolGroupAdapter.
cap_scheduler -> ToolGroupAdapter

// Lifecycle-only and direct commands stay in registry/router space.
Lifecycle != AgentAdapter
DirectCall != AgentAdapter

// GenericAgent never receives CapabilityRegistry.
CCapabilityRegistry -> ToolGroupAdapter -> ToolSet -> GenericAgent

// Channel aggregation belongs above orchestrator.
CCapabilityRegistry -> ChannelAdapter -> AgentSystem -> Orchestrator
```
