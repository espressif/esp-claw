# Channel

```rust
struct ChannelRegistry {
    channels: RwLock<HashMap<ChannelName, ChannelEntry>>,
    sink: ChannelSink,

    pub fn register(channel: Channel) -> Result<()> // validates, starts, then stores
    pub fn start_all() -> Result<()> // restarts stopped registered channels
    pub fn send(message: ChannelOutbound<'_>) -> Result<()> // dispatches by message.target.channel
    pub fn stop_all() -> Result<()> // stops runtime only; registration stays
}

struct ChannelEntry {
    channel: Channel,
    runtime: Option<ChannelRuntime>,
}

struct Channel {
    inner: Arc<ChannelInner>,

    pub fn from_handler(handler: impl ChannelHandler + 'static) -> Channel
    pub fn name(&self) -> &str // stable id used by routing and outbound target
    pub fn start(&self, sink: ChannelSink) -> Result<ChannelRuntime> // sync startup
    pub fn send(&self, message: ChannelOutbound<'_>) -> Result<()> // outbound delivery
}

trait ChannelHandler: Send + Sync {
    fn name(&self) -> &str // may return static text or borrow handler-owned metadata
    fn start(&self, sink: ChannelSink) -> Result<ChannelRuntime> // register/start_all boundary
    fn send(&self, message: ChannelOutbound<'_>) -> Result<()> // sends user-facing output
}

struct ChannelRuntime {
    pub fn stop(&self) -> Result<()> // shutdown for one started runtime
}

struct ChannelSink {
    pub fn submit(&self, input: ChannelInbound) -> Result<()> // channel -> claw-agent queue
}

struct ChannelInbound {
    pub channel: ChannelName // source channel id
    pub chat_id: String // source conversation endpoint
    pub text: Option<String> // natural-language user input when present
    pub attachments: Vec<ChannelAttachment> // user-facing media or files

    pub sender_id: Option<String> // platform sender id
    pub message_id: Option<String> // platform message id
    pub correlation_id: Option<String> // reply correlation key; defaults to message_id
    pub timestamp_ms: Option<i64> // source event time if available

    pub target: Option<ChannelTargetOwned> // explicit reply target; defaults to source
    pub content_type: Option<String> // text by default; preserves platform context
    pub payload_json: Option<String> // opaque platform/router metadata
}

struct ChannelTarget<'a> {
    pub channel: &'a str
    pub chat_id: &'a str
}

struct ChannelTargetOwned {
    pub channel: String
    pub chat_id: String
}

struct ChannelOutbound<'a> {
    pub target: ChannelTarget<'a> // explicit outbound endpoint
    pub text: Option<&'a str> // user-facing message content when present
    pub attachments: &'a [ChannelAttachment] // outbound media or files

    pub message_id: Option<&'a str> // outbound id when caller has one
    pub correlation_id: Option<&'a str> // links reply to inbound message
    pub payload_json: Option<&'a str> // opaque channel-specific outbound metadata
}

struct ChannelAttachment {
    pub kind: ChannelAttachmentKind
    pub path: Option<String> // local file path when the platform uploads bytes
    pub url: Option<String> // remote/download URL when already hosted
    pub name: Option<String> // display filename or short label
    pub mime_type: Option<String>
}

enum ChannelAttachmentKind {
    Image
    File
    Link
}

enum ChannelRegistryError {
    AlreadyExists(ChannelName)
    NotFound(ChannelName)
    InvalidChannel(ChannelName)
    StartFailed(ChannelName, ChannelError)
    SendFailed(ChannelName, ChannelError)
    StopFailed(ChannelName, ChannelError)
}

struct ChannelError {
    message: String
}
```

## Rules

```rust
// Channel is user-facing runtime I/O, not model-visible ability.
ChannelRegistry::register(channel) == start channel now

// There is no channel visibility state.
enable_channel(channel_id) // forbidden
disable_channel(channel_id) // forbidden

// There is channel lifecycle state.
start_all() // allowed; starts stopped registered channels
stop_all() // allowed; stops started channels

// There is no channel projection.
ChannelSet // forbidden
ChannelSetHandle // forbidden
ChannelProjection // forbidden
begin() // forbidden for channels

// Base agents and orchestrator never own channels.
Channel -> ChannelSink -> claw-agent -> OrchestratorSessionInput
OrchestratorSessionOutput -> claw-agent -> ChannelRegistry::send

// Channel is not a general event bus.
TriggerEvent // belongs to router/event system, not channel
```

## Register Flow

```rust
fn register(channel: Channel) -> Result<()> {
    let name = channel.name().to_owned()
    validate(name)
    reject_duplicate(name)

    let runtime = channel.start(sink.clone())? // sync; channel is live after this
    channels.insert(name, ChannelEntry { channel, runtime: Some(runtime) })
    Ok(())
}

fn start_all() -> Result<()> {
    for entry in channels.values_mut() {
        if entry.runtime.is_none() {
            entry.runtime = Some(entry.channel.start(sink.clone())?)
        }
    }
    Ok(())
}

fn stop_all() -> Result<()> {
    for entry in channels.values_mut() {
        if let Some(runtime) = entry.runtime.take() {
            runtime.stop()?
        }
    }
    Ok(())
}
```

## Inbound Flow

```rust
channel receives platform message
channel normalizes source fields into ChannelInbound
channel calls sink.submit(input)
claw-agent resolves session from channel/chat binding rules
claw-agent stores reply target from input.target or input source
claw-agent decides whether the input can become session text
claw-agent submits session_id + text to orchestrator when text is available
```

## Outbound Flow

```rust
orchestrator returns session output
claw-agent resolves ChannelTarget from session binding
claw-agent builds ChannelOutbound
claw-agent calls ChannelRegistry::send(outbound)
channel handler sends to platform
```

## Master Compatibility

```rust
// Existing master fields map directly.
claw_event.source_channel -> ChannelInbound.channel
claw_event.chat_id -> ChannelInbound.chat_id
claw_event.sender_id -> ChannelInbound.sender_id
claw_event.message_id -> ChannelInbound.message_id
claw_event.correlation_id -> ChannelInbound.correlation_id
claw_event.target_channel + target_endpoint -> ChannelInbound.target
claw_event.session_policy -> claw-agent session binding policy
claw_event.content_type -> ChannelInbound.content_type
claw_event.payload_json -> ChannelInbound.payload_json

// Existing core output maps directly.
response.target_channel + target_chat_id -> ChannelOutbound.target
response.text -> ChannelOutbound.text
request.source_message_id -> ChannelOutbound.correlation_id

// Existing IM attachment capabilities map directly.
send_image(path, caption) -> ChannelOutbound.attachments + text
send_file(path, caption) -> ChannelOutbound.attachments + text
link_url + link_label -> ChannelAttachment { kind: Link, url, name }
inbound attachment payload -> ChannelInbound.attachments + payload_json
```

## Difference From Tool

```rust
// Tool
ToolRegistry::register(tool) // catalog mutation
ToolRegistry::start_all() // tool lifecycle starts later
ToolSet::begin() // per-agent, per-iteration stable view
ToolSet::enable_tool() / ToolSet::disable_tool() // projection-local visibility state

// Channel
ChannelRegistry::register(channel) // starts immediately
ChannelRegistry::start_all() // starts stopped registered channels
ChannelRegistry::send(message) // runtime I/O
ChannelRegistry::stop_all() // stops runtime only; channel stays registered
```
