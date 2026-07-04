# Agent System

```rust
struct AgentSystem<F, H, Timer> {
    capabilities: Arc<CapabilityRegistry>,
    orchestrator: Arc<Orchestrator<F, H, Timer>>,
    inbox: AgentInbox,
    chat_sessions: Mutex<HashMap<ChannelChatKey, SessionId>>,
    session_targets: Mutex<HashMap<SessionId, SessionTarget>>,

    pub fn capabilities(&self) -> &CapabilityRegistry // registration surface for tools/channels
    pub fn start_all(&self) -> Result<()> // starts tools and stopped registered channels
    pub fn stop_all(&self) -> Result<()> // stops channels then tools

    pub async fn run_inbound(&self) -> Result<()> // drains ChannelSink messages
    pub async fn submit_channel(&self, input: ChannelInbound) -> Result<()> // direct inbound path

    pub fn new_session(&self) -> SessionId // explicit session creation
    pub fn bind_session(session: SessionId, target: ChannelTargetOwned) -> Result<()> // explicit binding
    pub fn list_sessions(&self) -> Vec<SessionRecord>
    pub fn delete_session(session: SessionId) -> Result<()>
}

struct AgentInbox {
    sender: AsyncSender<ChannelInbound>,
    receiver: AsyncReceiver<ChannelInbound>,

    pub fn sink(&self) -> ChannelSink // sync submit, no await in channel callback
}

struct ChannelChatKey {
    channel: String,
    chat_id: String,
}

struct SessionTarget {
    target: ChannelTargetOwned,
    correlation_id: Option<String>,
}

struct SessionRecord {
    id: SessionId,
    target: Option<ChannelTargetOwned>,
}
```

## Rules

```rust
// AgentSystem is the channel/session aggregation layer.
ChannelInbound -> AgentSystem -> Orchestrator::submit
DriveOutput -> AgentSystem -> ChannelRegistry::send

// Orchestrator never sees channels.
Orchestrator::submit(session_id, text, delivery_kind)

// Channel never sees sessions.
ChannelSink::submit(ChannelInbound)
ChannelRegistry::send(ChannelOutbound)

// Default IM/channel input is an interrupt.
default_delivery_kind(ChannelInbound) == DeliveryKind::Interrupt

// No transport hint decides DeliveryKind.
DeliveryKind::from_extra_context // forbidden in AgentSystem
extra_context // forbidden

// No router object with policy logic.
ChannelRouter // removed
```

## Construction

```rust
fn new(llm_config, persistence) -> Result<AgentSystem> {
    let inbox = AgentInbox::new()
    let capabilities = Arc::new(CapabilityRegistry::with_channel_sink(inbox.sink()))
    let resolver = RegistryResolver::new(Arc::clone(&capabilities))
    let orchestrator = Arc::new(Orchestrator::new(
        resolver,
        Arc::clone(&capabilities),
        llm_config,
        persistence,
    )?)

    Ok(AgentSystem {
        capabilities,
        orchestrator,
        inbox,
        chat_sessions: Mutex::new(HashMap::new()),
        session_targets: Mutex::new(HashMap::new()),
    })
}
```

## Inbound

```rust
fn ChannelSink::submit(input: ChannelInbound) -> Result<()> {
    inbox.sender.send(input)? // sync wake-up only
    Ok(())
}

async fn run_inbound(&self) -> Result<()> {
    while let Some(input) = inbox.receiver.recv().await {
        self.submit_channel(input).await?
    }
    Ok(())
}

async fn submit_channel(&self, input: ChannelInbound) -> Result<()> {
    validate_channel_input(&input)?
    let Some(text) = session_text(&input) else {
        remember_target_without_submit(input)
        return Ok(())
    }

    let session = resolve_or_create_session(&input)
    remember_target(session, &input)

    let output = orchestrator
        .submit(session, text, DeliveryKind::Interrupt)
        .await?

    surface_output(output).await
}
```

## Session Binding

```rust
fn resolve_or_create_session(input: &ChannelInbound) -> SessionId {
    let key = ChannelChatKey {
        channel: input.channel.clone(),
        chat_id: input.chat_id.clone(),
    }

    if let Some(session) = chat_sessions.get(&key) {
        return *session
    }

    let session = orchestrator.session_create()
    chat_sessions.insert(key, session)
    session
}

fn bind_session(session: SessionId, target: ChannelTargetOwned) -> Result<()> {
    validate_session_exists(session)?
    validate_channel_exists(&target.channel)?
    reject_conflicting_chat_binding(&target)?
    reject_conflicting_session_target(session, &target)?

    chat_sessions.insert(ChannelChatKey::from(&target), session)
    session_targets.insert(session, SessionTarget {
        target,
        correlation_id: None,
    })
    Ok(())
}

fn delete_session(session: SessionId) -> Result<()> {
    orchestrator.session_delete(session)?
    session_targets.remove(&session)
    chat_sessions.retain(|_, value| *value != session)
    Ok(())
}
```

## Target Memory

```rust
fn remember_target(session: SessionId, input: &ChannelInbound) {
    let target = input.target.clone().unwrap_or(ChannelTargetOwned {
        channel: input.channel.clone(),
        chat_id: input.chat_id.clone(),
    })

    session_targets.insert(session, SessionTarget {
        target,
        correlation_id: input.correlation_id.clone().or(input.message_id.clone()),
    })
}
```

## Outbound

```rust
async fn surface_output(&self, output: DriveOutput) -> Result<()> {
    for reply in output.replies {
        let target = session_targets
            .get(&reply.session)
            .cloned()
            .ok_or(Error::NoReplyTarget(reply.session))?

        capabilities.channel_registry().send(ChannelOutbound {
            target: ChannelTarget {
                channel: &target.target.channel,
                chat_id: &target.target.chat_id,
            },
            text: Some(&reply.text),
            attachments: &[],
            message_id: None,
            correlation_id: target.correlation_id.as_deref(),
            payload_json: None,
        })?
    }
    Ok(())
}
```

## Delivery Kind

```rust
fn default_delivery_kind(_input: &ChannelInbound) -> DeliveryKind {
    DeliveryKind::Interrupt
}
```

Rules:

```rust
// Channel/user message default.
ChannelInbound -> DeliveryKind::Interrupt

// Append is only for explicit internal/session APIs.
append_to_session(session, text) -> DeliveryKind::Append

// Cancel is only for explicit hard-stop APIs.
cancel_session(session, text) -> DeliveryKind::Cancel
```

Reason:

```rust
// IM input usually means the human is superseding the current work.
// Interrupt lets the current tool call / iteration finish and commit.
// The new message is recorded as an interruption, not silently queued.
// Cancel is too destructive for ordinary chat input.
// Append waits behind active work and is wrong as the channel default.
```

## Async

```rust
// Channel callbacks never await.
ChannelSink::submit(input) // sync enqueue + wake

// AgentSystem awaits only after all binding locks are released.
resolve_or_create_session(input) // lock only here
remember_target(session, input) // lock only here
orchestrator.submit(...).await // no AgentSystem lock held
ChannelRegistry::send(outbound) // no AgentSystem lock held
```

## Replacement

```rust
// Remove old claw-agent channel abstractions.
InboundMessage // replaced by ChannelInbound
OutboundMessage // replaced by ChannelOutbound
ChannelAdapter // replaced by ChannelHandler
ChannelRuntime // replaced by ChannelSink + channel::ChannelRuntime
Registry // replaced by CapabilityRegistry
ChannelRouter // deleted
```
