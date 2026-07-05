# Agent System

```rust
struct AgentSystem<F, H, Timer> {
    capabilities: Arc<CapabilityRegistry>,
    orchestrator: Arc<Orchestrator<F, H, Timer>>,
    session_targets: Mutex<HashMap<SessionId, SessionTarget>>,

    pub fn capabilities(&self) -> &CapabilityRegistry // registration surface for tools/channels
    pub fn start_all(&self) -> Result<()> // starts tools and stopped registered channels
    pub fn stop_all(&self) -> Result<()> // stops channels then tools

    pub async fn submit_channel(&self, session: SessionId, input: ChannelInbound) -> Result<()>

    pub fn new_session(&self) -> SessionId // explicit session creation
    pub fn list_sessions(&self) -> Vec<SessionRecord>
    pub fn delete_session(session: SessionId) -> Result<()>
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
SessionId + ChannelInbound -> AgentSystem -> Orchestrator::submit
DriveOutput -> AgentSystem -> ChannelRegistry::send

// Orchestrator never sees channels.
Orchestrator::submit(session_id, text, delivery_kind)

// Channel never sees sessions.
ChannelSink::submit(ChannelInbound)
ChannelRegistry::send(ChannelOutbound)

// Default IM/channel input is an interrupt.
default_delivery_kind(ChannelInbound) == DeliveryKind::Interrupt

// Session binding policy is not hardcoded here.
ChannelChatKey -> SessionId // belongs to embedding adapter or claw-cabi
resolve_or_create_session // forbidden in AgentSystem

// No transport hint decides DeliveryKind.
DeliveryKind::from_extra_context // forbidden in AgentSystem
extra_context // forbidden

// No router object with policy logic.
ChannelRouter // removed
```

## Construction

```rust
fn new(llm_config, persistence) -> Result<AgentSystem> {
    let capabilities = Arc::new(CapabilityRegistry::new())
    let orchestrator = Arc::new(Orchestrator::new(
        Arc::clone(&capabilities),
        llm_config,
        persistence,
    )?)

    Ok(AgentSystem {
        capabilities,
        orchestrator,
        session_targets: Mutex::new(HashMap::new()),
    })
}
```

## Agent Resolution

```rust
struct FsAgentFactory {
    capabilities: Arc<CapabilityRegistry>,

    fn create_agent(kind: AgentKind, ...) -> Result<Agent> {
        let manifest = AgentManifest::for_kind(kind)?
        let mut tools = capabilities.tool_set()

        for name in manifest.capabilities {
            let tool = bind_tool(name)? // factory-local manifest binding
            tools.add_tool(tool)?
        }

        let skills = resolve_skills(manifest.skills)?
        GenericAgent::new(..., tools, skills, ...)
    }
}
```

Rules:

```rust
// There is no manifest binding injection above factory.
AgentSystem::new(resolver, ...) // forbidden
Orchestrator::new(resolver, ...) // forbidden

// Factory is where baked manifest names become runtime objects.
AgentManifest.capabilities -> FsAgentFactory::bind_tool -> ToolSet.add_tool
AgentManifest.skills -> FsAgentFactory::bind_skills -> SkillSet

// The resolved result is used immediately for this agent.
bind_tool(name) -> Tool
tools.add_tool(tool)
```

## Inbound

```rust
async fn submit_channel(&self, session: SessionId, input: ChannelInbound) -> Result<()> {
    validate_session_exists(session)?
    validate_channel_input(&input)?
    let Some(text) = session_text(&input) else {
        remember_target(session, &input)
        return Ok(())
    }

    remember_target(session, &input)

    let output = orchestrator
        .submit(session, text, DeliveryKind::Interrupt)
        .await?

    surface_output(output).await
}
```

## Session Binding

```rust
fn delete_session(session: SessionId) -> Result<()> {
    orchestrator.session_delete(session)?
    session_targets.remove(&session)
    Ok(())
}
```

Rules:

```rust
// AgentSystem owns sessions, not binding policy.
new_session() // creates an orchestrator session
submit_channel(session, input) // caller already chose the session

// These are not AgentSystem APIs.
bind_session(channel, chat_id, session) // forbidden here
auto_bind(channel, chat_id) // forbidden here
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
// AgentSystem awaits only after all binding locks are released.
remember_target(session, input) // lock only here
orchestrator.submit(...).await // no AgentSystem lock held
ChannelRegistry::send(outbound) // no AgentSystem lock held
```

## CABI Auto Bind

```rust
struct CabiChannelQueue {
    sender: AsyncSender<ChannelInbound>,
    receiver: AsyncReceiver<ChannelInbound>,

    pub fn new() -> CabiChannelQueue
    pub fn sink(&self) -> ChannelSink // passed to CapabilityRegistry before AgentSystem exists
}

struct CabiChannelBridge<F, H, Timer> {
    system: Arc<AgentSystem<F, H, Timer>>,
    bindings: Mutex<HashMap<ChannelChatKey, SessionId>>,
    receiver: AsyncReceiver<ChannelInbound>,

    pub async fn run(&self) -> Result<()> // drains channel messages
}

struct ChannelChatKey {
    channel: String,
    chat_id: String,
}

fn CabiChannelQueue::sink(&self) -> ChannelSink {
    let sender = self.sender.clone()
    ChannelSink::new(move |input| {
        sender.send(input).map_err(|_| ChannelError::new("agent runtime stopped"))
    })
}

async fn CabiChannelBridge::run(&self) -> Result<()> {
    while let Some(input) = receiver.recv().await {
        let session = self.resolve_or_create_session(&input)
        system.submit_channel(session, input).await?
    }
    Ok(())
}

fn resolve_or_create_session(&self, input: &ChannelInbound) -> SessionId {
    let key = ChannelChatKey {
        channel: input.channel.clone(),
        chat_id: input.chat_id.clone(),
    }

    if let Some(session) = bindings.get(&key) {
        return *session
    }

    let session = system.new_session()
    bindings.insert(key, session)
    session
}
```

Rules:

```rust
// ESP-IDF C API chooses auto-bind behavior.
claw-cabi inbound message -> CabiChannelBridge::resolve_or_create_session

// Other embeddings can choose another binding policy.
host test -> explicit session selection
app server -> account/thread/router-specific session selection
```

## CABI Executor

```rust
struct CabiAgentRuntime<F, H, Timer> {
    system: Arc<AgentSystem<F, H, Timer>>,
    bridge: Arc<CabiChannelBridge<F, H, Timer>>,
    worker: WorkerHandle,
}

fn start(config) -> Result<CabiAgentRuntime> {
    let queue = CabiChannelQueue::new()

    let system = Arc::new(AgentSystem::new(
        config.llm,
        config.persistence,
    )?)
    register_c_capabilities(system.capabilities())?

    let bridge = Arc::new(CabiChannelBridge {
        system: Arc::clone(&system),
        bindings: Mutex::new(HashMap::new()),
        receiver: queue.receiver,
    })

    let bridge_for_worker = Arc::clone(&bridge)
    let worker = EspIdfThread.spawn_worker(move || {
        let executor = edge_executor::LocalExecutor::new()
        let task = executor.spawn(bridge_for_worker.run())
        edge_executor::block_on(executor.run(task))
    })?

    system.start_all()?

    Ok(CabiAgentRuntime { system, bridge, worker })
}
```

Rules:

```rust
// AgentSystem never owns an executor.
AgentSystem::submit_channel(...) // async API only

// claw-cabi owns the ESP-IDF worker and edge executor.
claw-cabi worker -> edge_executor -> CabiChannelBridge::run

// C callbacks never await.
C callback -> ChannelSink::submit -> AsyncSender::send

// The queue may receive before the executor polls.
AsyncSender<ChannelInbound> // buffers until bridge.run() drains

// No AgentSystem lock is held while the executor awaits.
CabiChannelBridge::resolve_or_create_session // lock only here
AgentSystem::submit_channel(...).await // no bridge lock held
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
