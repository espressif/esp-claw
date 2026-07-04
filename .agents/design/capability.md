# Capability

```rust
enum Capability {
    Tool(Tool)
    Channel(Channel)
}

struct CapabilityRegistry {
    tools: ToolRegistry,
    channels: ChannelRegistry,

    pub fn register(capability: Capability) -> Result<()> // dispatches to ToolRegistry or ChannelRegistry
    pub fn tool_set() -> ToolSet // ToolSet must come from the registry
    pub fn channel_registry() -> &ChannelRegistry // claw-agent owns channel aggregation

    pub fn start_all() -> Result<()> // starts tools and stopped registered channels
    pub fn stop_all() -> Result<()> // stops started channels and tools
}
```

## Rules

```rust
// CapabilityRegistry is only the composition root.
ToolRegistry // owns tool catalog, visibility, projection source
ChannelRegistry // owns channel runtime lifecycle and outbound delivery

// Registration is one API.
register(Capability::Tool(tool)) // tool registration is catalog mutation
register(Capability::Channel(channel)) // channel registration starts runtime I/O

// CapabilityRegistry has no visibility API.
enable_tool() // forbidden here
disable_tool() // forbidden here
enable_channel() // forbidden
disable_channel() // forbidden

// Start/stop is lifecycle, not visibility.
start_all() // delegates to tools and channels
stop_all() // delegates to channels and tools
```

## Register

```rust
fn register(capability: Capability) -> Result<()> {
    match capability {
        Capability::Tool(tool) => tools.register(tool),
        Capability::Channel(channel) => channels.register(channel),
    }
}
```

## Lifecycle

```rust
fn start_all() -> Result<()> {
    tools.start_all()?
    channels.start_all()?
    Ok(())
}

fn stop_all() -> Result<()> {
    channels.stop_all()?
    tools.stop_all()?
    Ok(())
}
```
