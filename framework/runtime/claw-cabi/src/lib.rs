//! `claw-cabi` — the single outbound C ABI (Rust -> C) for the agent/capability
//! stack.
//!
//! To C, the Rust side exposes two surfaces:
//!
//! - **Control plane** — register capabilities:
//!   [`claw_capability_register`] / [`claw_capability_register_group`].
//! - **Data plane** — C submits channel messages through
//!   [`claw_agent_system_push_message`] or, for registered C channel
//!   capabilities, through the [`ClawChannelRuntime`] handed to the channel's
//!   open callback.
//! - **Agent system plane** (ESP-IDF target only) — create/start/stop/destroy a
//!   device agent runtime and expose explicit session create/bind/list/delete
//!   calls for C callers.
//!
//! `claw-agent` remains the Rust-native API. It builds an [`AgentSystem`] and
//! exposes async channel submission directly for host/dev callers. This crate is the
//! C adapter: on ESP-IDF it selects the concrete platform backends, starts a
//! worker with `edge_executor`, and translates C's synchronous calls into
//! commands for the async agent system.
//!
//! This is the one crate in the workspace where `unsafe` / `extern "C"` is
//! allowed; every other crate keeps `unsafe_code = "forbid"`. Every `extern "C"`
//! body runs under a panic guard so unwinding never crosses into C; strings are
//! borrowed both ways (see `result`).

mod abi;
mod result;
mod wrappers;

use core::future::Future;
use core::task::{Context, Poll};
use std::sync::Arc;
#[cfg(target_os = "espidf")]
use std::sync::{
    atomic::{AtomicBool, Ordering},
    mpsc, Mutex,
};
use std::task::{Wake, Waker};

#[cfg(target_os = "espidf")]
use claw_agent::{
    init_tool_executor, AgentPersistenceConfig, AgentSystem, BackendKind, ClawApiConfig, SessionId,
    SessionRecord,
};
use claw_agent::{CapabilityError, ChannelRuntime, Registry};
#[cfg(target_os = "espidf")]
use claw_agent::{ChannelFuture, InboundMessage};
#[cfg(target_os = "espidf")]
use claw_interface::{ClawThread, CoreAffinity, Priority, WorkerHandle};
#[cfg(target_os = "espidf")]
use claw_sys::{EspIdfFs, EspIdfHttp, EspIdfThread, EspIdfTimer};
#[cfg(target_os = "espidf")]
use claw_utils::{async_channel, AsyncReceiver, AsyncSender};
#[cfg(target_os = "espidf")]
use core::{
    ffi::{c_char, c_void, CStr},
    ptr,
};
#[cfg(target_os = "espidf")]
use std::ffi::CString;

pub use abi::{
    ClawAgentSessionListCallback, ClawAgentSessionRecord, ClawAgentSystemConfig, ClawCapability,
    ClawCapabilityChannel, ClawCapabilityChannelCloseCallback, ClawCapabilityChannelOpenCallback,
    ClawCapabilityExecuteCallback, ClawCapabilityGroup, ClawCapabilityLifecycle,
    ClawCapabilityLifecycleCallback, ClawCapabilityRole, ClawCapabilityRoleData,
    ClawCapabilitySendCallback, ClawCapabilityTool, ClawInboundMessage,
    CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY,
};
pub use result::{ClawCapabilityErrorKind, ClawCapabilityResult};

#[cfg(target_os = "espidf")]
use crate::result::into_result;
use crate::result::{from_result, guard};
use crate::wrappers::{build_capability, build_group, build_inbound};

struct NoopWake;

impl Wake for NoopWake {
    fn wake(self: Arc<Self>) {}
}

fn block_on<F: Future>(future: F) -> F::Output {
    let mut future = Box::pin(future);
    let waker = Waker::from(Arc::new(NoopWake));
    let mut context = Context::from_waker(&waker);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
    }
}

#[cfg(target_os = "espidf")]
const AGENT_WORKER_STACK_SIZE: usize = 64 * 1024;
#[cfg(target_os = "espidf")]
const SESSION_ID_BUFFER_MIN_CAPACITY: usize = 32;

#[cfg(target_os = "espidf")]
#[derive(Clone)]
struct AgentRuntimeConfig {
    llm: ClawApiConfig,
    persistence: AgentPersistenceConfig,
    registry: Arc<Registry>,
}

#[cfg(target_os = "espidf")]
enum RuntimeCommand {
    Inbound(InboundMessage),
    SessionCreate {
        reply: mpsc::Sender<Result<SessionId, CapabilityError>>,
    },
    SessionList {
        reply: mpsc::Sender<Result<Vec<SessionRecord>, CapabilityError>>,
    },
    SessionBind {
        session: SessionId,
        channel: String,
        chat_id: String,
        reply: mpsc::Sender<Result<(), CapabilityError>>,
    },
    SessionDelete {
        session: SessionId,
        reply: mpsc::Sender<Result<(), CapabilityError>>,
    },
    Shutdown,
}

#[cfg(target_os = "espidf")]
struct AgentRuntime {
    sender: Arc<Mutex<AsyncSender<RuntimeCommand>>>,
    receiver: Option<AsyncReceiver<RuntimeCommand>>,
    running: Arc<AtomicBool>,
    worker: Option<WorkerHandle>,
}

#[cfg(target_os = "espidf")]
impl AgentRuntime {
    fn new() -> Self {
        let (sender, receiver) = async_channel();
        Self {
            sender: Arc::new(Mutex::new(sender)),
            receiver: Some(receiver),
            running: Arc::new(AtomicBool::new(false)),
            worker: None,
        }
    }

    fn is_running(&self) -> bool {
        self.running.load(Ordering::Acquire)
    }

    fn start(&mut self, config: AgentRuntimeConfig) -> Result<(), CapabilityError> {
        if self.is_running() || self.worker.is_some() {
            return Err(CapabilityError::InvalidState);
        }
        let receiver = self.receiver.take().ok_or(CapabilityError::InvalidState)?;
        let command_sender = Arc::clone(&self.sender);
        let (ready_sender, ready_receiver) = mpsc::channel();
        let running = Arc::clone(&self.running);
        self.running.store(true, Ordering::Release);
        let handle = EspIdfThread
            .spawn_worker(
                "claw_agent_async",
                AGENT_WORKER_STACK_SIZE,
                Priority::Normal,
                CoreAffinity::Any,
                move || run_agent_executor(config, receiver, command_sender, ready_sender, running),
            )
            .map_err(|error| {
                self.running.store(false, Ordering::Release);
                let _ = self.reset_command_channel();
                CapabilityError::Failed(error.to_string())
            })?;
        self.worker = Some(handle);

        match ready_receiver.recv() {
            Ok(Ok(())) => Ok(()),
            Ok(Err(error)) => {
                self.join_worker_and_reset()?;
                Err(error)
            }
            Err(_) => {
                self.join_worker_and_reset()?;
                Err(CapabilityError::Failed(
                    "agent runtime stopped before it became ready".to_string(),
                ))
            }
        }
    }

    fn stop(&mut self) -> Result<(), CapabilityError> {
        if self.is_running() {
            let _ = self.send_command(RuntimeCommand::Shutdown);
        }
        self.join_worker_and_reset()
    }

    fn send_command(&self, command: RuntimeCommand) -> Result<(), CapabilityError> {
        if !self.is_running() {
            return Err(CapabilityError::InvalidState);
        }
        self.sender
            .lock()
            .map_err(|_| CapabilityError::Failed("agent runtime sender lock poisoned".to_string()))?
            .send(command)
            .map_err(|_| CapabilityError::InvalidState)
    }

    fn reset_command_channel(&mut self) -> Result<(), CapabilityError> {
        let (sender, receiver) = async_channel();
        *self.sender.lock().map_err(|_| {
            CapabilityError::Failed("agent runtime sender lock poisoned".to_string())
        })? = sender;
        self.receiver = Some(receiver);
        Ok(())
    }

    fn join_worker_and_reset(&mut self) -> Result<(), CapabilityError> {
        if let Some(worker) = self.worker.take() {
            worker.join();
        }
        self.running.store(false, Ordering::Release);
        self.reset_command_channel()
    }
}

#[cfg(target_os = "espidf")]
struct RuntimeCommandSink {
    sender: Arc<Mutex<AsyncSender<RuntimeCommand>>>,
    running: Arc<AtomicBool>,
}

#[cfg(target_os = "espidf")]
impl ChannelRuntime for RuntimeCommandSink {
    fn push_message(&self, message: InboundMessage) -> ChannelFuture<'_> {
        Box::pin(async move {
            if !self.running.load(Ordering::Acquire) {
                return Err(CapabilityError::InvalidState);
            }
            self.sender
                .lock()
                .map_err(|_| {
                    CapabilityError::Failed("agent runtime sender lock poisoned".to_string())
                })?
                .send(RuntimeCommand::Inbound(message))
                .map_err(|_| CapabilityError::InvalidState)
        })
    }
}

#[cfg(target_os = "espidf")]
fn run_agent_executor(
    config: AgentRuntimeConfig,
    receiver: AsyncReceiver<RuntimeCommand>,
    sender: Arc<Mutex<AsyncSender<RuntimeCommand>>>,
    ready_sender: mpsc::Sender<Result<(), CapabilityError>>,
    running: Arc<AtomicBool>,
) {
    let channel_runtime = Arc::new(RuntimeCommandSink {
        sender,
        running: Arc::clone(&running),
    }) as Arc<dyn ChannelRuntime>;
    let system = match build_agent_system(&config) {
        Ok(system) => match system.start_with_runtime(channel_runtime) {
            Ok(()) => {
                let _ = ready_sender.send(Ok(()));
                system
            }
            Err(error) => {
                let _ = ready_sender.send(Err(error));
                running.store(false, Ordering::Release);
                return;
            }
        },
        Err(error) => {
            let _ = ready_sender.send(Err(error));
            running.store(false, Ordering::Release);
            return;
        }
    };
    let executor: edge_executor::LocalExecutor = Default::default();
    let task = executor.spawn(agent_worker_loop(system, receiver));
    edge_executor::block_on(executor.run(task));
    running.store(false, Ordering::Release);
}

#[cfg(target_os = "espidf")]
fn build_agent_system(config: &AgentRuntimeConfig) -> Result<AgentSystem, CapabilityError> {
    AgentSystem::new::<EspIdfFs, EspIdfHttp, EspIdfTimer>(
        config.llm.clone(),
        config.persistence.clone(),
        Arc::clone(&config.registry),
    )
    .map_err(|error| CapabilityError::Failed(error.to_string()))
}

#[cfg(target_os = "espidf")]
async fn agent_worker_loop(system: AgentSystem, receiver: AsyncReceiver<RuntimeCommand>) {
    while let Some(command) = receiver.recv().await {
        match command {
            RuntimeCommand::Inbound(message) => {
                let _ = system.push_message(message).await;
            }
            RuntimeCommand::SessionCreate { reply } => {
                let _ = reply.send(Ok(system.new_session()));
            }
            RuntimeCommand::SessionList { reply } => {
                let _ = reply.send(Ok(system.list_sessions()));
            }
            RuntimeCommand::SessionBind {
                session,
                channel,
                chat_id,
                reply,
            } => {
                let _ = reply.send(system.bind_session(session, &channel, &chat_id));
            }
            RuntimeCommand::SessionDelete { session, reply } => {
                let _ = reply.send(system.delete_session(session));
            }
            RuntimeCommand::Shutdown => {
                let _ = system.stop();
                break;
            }
        }
    }
}

/// Opaque agent runtime handle.
#[cfg(target_os = "espidf")]
pub struct ClawAgentSystem {
    config: AgentRuntimeConfig,
    runtime: Mutex<AgentRuntime>,
}

/// Opaque registry handle: wraps `Arc<claw_agent::Registry>`.
///
/// Created and destroyed Rust-side (the firmware's Rust entry point owns the
/// `Registry`); C only receives the pointer to register into it.
pub struct ClawCapabilityRegistry {
    registry: Arc<Registry>,
}

impl ClawCapabilityRegistry {
    /// Box a shared registry into a raw handle for C. The matching destructor is
    /// [`drop_raw`](Self::drop_raw).
    pub fn into_raw(registry: Arc<Registry>) -> *mut ClawCapabilityRegistry {
        Box::into_raw(Box::new(ClawCapabilityRegistry { registry }))
    }

    /// Reclaim and drop a handle produced by [`into_raw`](Self::into_raw).
    ///
    /// # Safety
    /// `handle` must be null or a pointer returned by [`into_raw`](Self::into_raw)
    /// that has not already been dropped.
    pub unsafe fn drop_raw(handle: *mut ClawCapabilityRegistry) {
        if !handle.is_null() {
            drop(Box::from_raw(handle));
        }
    }
}

/// Create an empty capability registry for C registration.
///
/// # Safety
/// `ret_registry` must be a valid out-pointer.
#[no_mangle]
pub unsafe extern "C" fn claw_capability_registry_create(
    ret_registry: *mut *mut ClawCapabilityRegistry,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { registry_create_inner(ret_registry) }))
}

unsafe fn registry_create_inner(
    ret_registry: *mut *mut ClawCapabilityRegistry,
) -> Result<(), CapabilityError> {
    let out = ret_registry.as_mut().ok_or(CapabilityError::InvalidArg)?;
    *out = core::ptr::null_mut();
    *out = ClawCapabilityRegistry::into_raw(Arc::new(Registry::new()));
    Ok(())
}

/// Destroy a registry handle previously returned by
/// [`claw_capability_registry_create`].
///
/// # Safety
/// `registry` must be null or a live registry handle not already destroyed.
#[no_mangle]
pub unsafe extern "C" fn claw_capability_registry_destroy(
    registry: *mut ClawCapabilityRegistry,
) -> ClawCapabilityResult {
    guard(|| {
        unsafe { ClawCapabilityRegistry::drop_raw(registry) };
        crate::result::ok()
    })
}

/// Opaque channel runtime handle passed to C channel `open` callbacks.
pub struct ClawChannelRuntime {
    runtime: Arc<dyn ChannelRuntime>,
}

impl ClawChannelRuntime {
    pub fn into_raw(runtime: Arc<dyn ChannelRuntime>) -> *mut Self {
        Box::into_raw(Box::new(Self { runtime }))
    }

    /// # Safety
    /// `handle` must be null or a pointer returned by [`into_raw`](Self::into_raw)
    /// that has not already been dropped.
    pub unsafe fn drop_raw(handle: *mut Self) {
        if !handle.is_null() {
            drop(Box::from_raw(handle));
        }
    }
}

/// Destroy a channel runtime handle previously handed to a C channel.
///
/// # Safety
/// `runtime` must be null or a live runtime handle not already destroyed.
#[no_mangle]
pub unsafe extern "C" fn claw_channel_runtime_destroy(
    runtime: *mut ClawChannelRuntime,
) -> ClawCapabilityResult {
    guard(|| {
        unsafe { ClawChannelRuntime::drop_raw(runtime) };
        crate::result::ok()
    })
}

/// Submit one inbound channel message through a channel runtime handle.
///
/// # Safety
/// `runtime` must be a live handle and `message` a valid `ClawInboundMessage`.
#[no_mangle]
pub unsafe extern "C" fn claw_channel_runtime_push(
    runtime: *mut ClawChannelRuntime,
    message: *const ClawInboundMessage,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { channel_runtime_push_inner(runtime, message) }))
}

unsafe fn channel_runtime_push_inner(
    runtime: *mut ClawChannelRuntime,
    message: *const ClawInboundMessage,
) -> Result<(), CapabilityError> {
    let runtime = runtime.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let descriptor = message.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let inbound = unsafe { build_inbound(descriptor)? };
    block_on(runtime.runtime.push_message(inbound))
}

#[cfg(target_os = "espidf")]
/// Build an ESP-IDF agent runtime from an already-populated registry.
///
/// This does not start the worker; call [`claw_agent_system_start`] next.
///
/// # Safety
/// All pointers must be valid for the duration of the call. `registry` must be a
/// live registry handle. `ret_system` must be a valid out-pointer.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_create(
    config: *const ClawAgentSystemConfig,
    registry: *mut ClawCapabilityRegistry,
    ret_system: *mut *mut ClawAgentSystem,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { agent_system_create_inner(config, registry, ret_system) }))
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_create_inner(
    config: *const ClawAgentSystemConfig,
    registry: *mut ClawCapabilityRegistry,
    ret_system: *mut *mut ClawAgentSystem,
) -> Result<(), CapabilityError> {
    let config = config.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let registry_handle = registry.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let system_out = ret_system.as_mut().ok_or(CapabilityError::InvalidArg)?;
    *system_out = core::ptr::null_mut();

    let registry = Arc::clone(&registry_handle.registry);
    let llm = build_llm_config(config)?;
    let persistence_dir = unsafe { required_string(config.persistence_dir)? };
    let persistence = AgentPersistenceConfig::new(&persistence_dir);
    init_tool_executor(EspIdfThread).map_err(|error| CapabilityError::Failed(error.to_string()))?;

    let runtime_config = AgentRuntimeConfig {
        llm,
        persistence,
        registry,
    };

    let runtime = AgentRuntime::new();

    *system_out = Box::into_raw(Box::new(ClawAgentSystem {
        config: runtime_config,
        runtime: Mutex::new(runtime),
    }));
    Ok(())
}

#[cfg(target_os = "espidf")]
/// Start all registered capability lifecycles for this runtime.
///
/// # Safety
/// `system` must be a live system handle.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_start(
    system: *mut ClawAgentSystem,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { agent_system_start_inner(system) }))
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_start_inner(system: *mut ClawAgentSystem) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    handle
        .runtime
        .lock()
        .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?
        .start(handle.config.clone())
}

#[cfg(target_os = "espidf")]
/// Stop all registered capability lifecycles for this runtime.
///
/// # Safety
/// `system` must be a live system handle.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_stop(
    system: *mut ClawAgentSystem,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { agent_system_stop_inner(system) }))
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_stop_inner(system: *mut ClawAgentSystem) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    handle
        .runtime
        .lock()
        .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?
        .stop()
}

#[cfg(target_os = "espidf")]
/// Destroy an agent runtime handle, stopping lifecycles first.
///
/// # Safety
/// `system` must be null or a live system handle not already destroyed.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_destroy(
    system: *mut ClawAgentSystem,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { agent_system_destroy_inner(system) }))
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_destroy_inner(system: *mut ClawAgentSystem) -> Result<(), CapabilityError> {
    if system.is_null() {
        return Ok(());
    }
    let boxed = Box::from_raw(system);
    if let Ok(mut runtime) = boxed.runtime.lock() {
        let _ = runtime.stop();
    }
    drop(boxed);
    Ok(())
}

#[cfg(target_os = "espidf")]
/// Submit one inbound channel message to the running agent system.
///
/// # Safety
/// `system` must be a live system handle and `message` a valid
/// [`ClawInboundMessage`] for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_push_message(
    system: *mut ClawAgentSystem,
    message: *const ClawInboundMessage,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { agent_system_push_message_inner(system, message) }))
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_push_message_inner(
    system: *mut ClawAgentSystem,
    message: *const ClawInboundMessage,
) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let descriptor = message.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let inbound = unsafe { build_inbound(descriptor)? };
    handle
        .runtime
        .lock()
        .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?
        .send_command(RuntimeCommand::Inbound(inbound))
}

#[cfg(target_os = "espidf")]
/// Create a fresh conversation session and copy its wire id into
/// `session_id_buffer`.
///
/// # Safety
/// `system` must be a live system handle. `session_id_buffer` must be writable
/// for `session_id_capacity` bytes, and `session_id_length` must be a valid
/// out-pointer.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_session_create(
    system: *mut ClawAgentSystem,
    session_id_buffer: *mut c_char,
    session_id_capacity: usize,
    session_id_length: *mut usize,
) -> ClawCapabilityResult {
    guard(|| {
        from_result(unsafe {
            agent_system_session_create_inner(
                system,
                session_id_buffer,
                session_id_capacity,
                session_id_length,
            )
        })
    })
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_session_create_inner(
    system: *mut ClawAgentSystem,
    session_id_buffer: *mut c_char,
    session_id_capacity: usize,
    session_id_length: *mut usize,
) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    validate_session_id_output_buffer(session_id_buffer, session_id_capacity, session_id_length)?;
    let (reply_tx, reply_rx) = mpsc::channel();
    {
        let runtime = handle
            .runtime
            .lock()
            .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?;
        runtime.send_command(RuntimeCommand::SessionCreate { reply: reply_tx })?;
    }
    let session = reply_rx.recv().map_err(|_| {
        CapabilityError::Failed("agent runtime stopped before creating session".to_string())
    })??;
    copy_string_to_c_buffer(
        &session.to_wire(),
        session_id_buffer,
        session_id_capacity,
        session_id_length,
    )
}

#[cfg(target_os = "espidf")]
/// Bind an existing conversation session to one external channel chat.
///
/// Inbound channel messages for `(channel, chat_id)` are accepted only after
/// this explicit binding.
///
/// # Safety
/// `system` must be a live system handle. `session_id`, `channel`, and `chat_id`
/// must be valid NUL-terminated UTF-8 strings.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_session_bind(
    system: *mut ClawAgentSystem,
    session_id: *const c_char,
    channel: *const c_char,
    chat_id: *const c_char,
) -> ClawCapabilityResult {
    guard(|| {
        from_result(unsafe {
            agent_system_session_bind_inner(system, session_id, channel, chat_id)
        })
    })
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_session_bind_inner(
    system: *mut ClawAgentSystem,
    session_id: *const c_char,
    channel: *const c_char,
    chat_id: *const c_char,
) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let session = required_session_id(session_id)?;
    let channel = required_string(channel)?;
    let chat_id = required_string(chat_id)?;
    let (reply_tx, reply_rx) = mpsc::channel();
    {
        let runtime = handle
            .runtime
            .lock()
            .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?;
        runtime.send_command(RuntimeCommand::SessionBind {
            session,
            channel,
            chat_id,
            reply: reply_tx,
        })?;
    }
    reply_rx.recv().map_err(|_| {
        CapabilityError::Failed("agent runtime stopped before binding session".to_string())
    })?
}

#[cfg(target_os = "espidf")]
/// Enumerate live conversation sessions.
///
/// The callback is invoked once for each live session. Pointers inside
/// [`ClawAgentSessionRecord`] are borrowed and valid only until the callback
/// returns.
///
/// # Safety
/// `system` must be a live system handle. `callback` must be non-null and must
/// not retain borrowed record pointers.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_session_list(
    system: *mut ClawAgentSystem,
    callback: Option<ClawAgentSessionListCallback>,
    user_context: *mut c_void,
) -> ClawCapabilityResult {
    guard(|| {
        from_result(unsafe { agent_system_session_list_inner(system, callback, user_context) })
    })
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_session_list_inner(
    system: *mut ClawAgentSystem,
    callback: Option<ClawAgentSessionListCallback>,
    user_context: *mut c_void,
) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let callback = callback.ok_or(CapabilityError::InvalidArg)?;
    let (reply_tx, reply_rx) = mpsc::channel();
    {
        let runtime = handle
            .runtime
            .lock()
            .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?;
        runtime.send_command(RuntimeCommand::SessionList { reply: reply_tx })?;
    }
    let sessions = reply_rx.recv().map_err(|_| {
        CapabilityError::Failed("agent runtime stopped before listing sessions".to_string())
    })??;

    for session in sessions {
        unsafe { call_session_list_callback(&session, callback, user_context)? };
    }
    Ok(())
}

#[cfg(target_os = "espidf")]
unsafe fn call_session_list_callback(
    session: &SessionRecord,
    callback: ClawAgentSessionListCallback,
    user_context: *mut c_void,
) -> Result<(), CapabilityError> {
    let session_id = CString::new(session.id.to_wire())
        .map_err(|_| CapabilityError::Failed("session id contains interior nul".to_string()))?;
    let channel = optional_c_string(session.channel.as_deref(), "session channel")?;
    let chat_id = optional_c_string(session.chat_id.as_deref(), "session chat id")?;
    let record = ClawAgentSessionRecord {
        session_id: session_id.as_ptr(),
        channel: channel.as_ref().map_or(ptr::null(), |value| value.as_ptr()),
        chat_id: chat_id.as_ref().map_or(ptr::null(), |value| value.as_ptr()),
    };
    unsafe { into_result(callback(&record, user_context)) }
}

#[cfg(target_os = "espidf")]
fn optional_c_string(value: Option<&str>, label: &str) -> Result<Option<CString>, CapabilityError> {
    value
        .map(|value| {
            CString::new(value)
                .map_err(|_| CapabilityError::Failed(format!("{label} contains interior nul")))
        })
        .transpose()
}

#[cfg(target_os = "espidf")]
/// Delete a conversation session and drop its live agent graph.
///
/// # Safety
/// `system` must be a live system handle. `session_id` must be a valid
/// NUL-terminated UTF-8 `session-N` string.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_session_delete(
    system: *mut ClawAgentSystem,
    session_id: *const c_char,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { agent_system_session_delete_inner(system, session_id) }))
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_session_delete_inner(
    system: *mut ClawAgentSystem,
    session_id: *const c_char,
) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let session = required_session_id(session_id)?;
    let (reply_tx, reply_rx) = mpsc::channel();
    {
        let runtime = handle
            .runtime
            .lock()
            .map_err(|_| CapabilityError::Failed("agent runtime lock poisoned".to_string()))?;
        runtime.send_command(RuntimeCommand::SessionDelete {
            session,
            reply: reply_tx,
        })?;
    }
    reply_rx.recv().map_err(|_| {
        CapabilityError::Failed("agent runtime stopped before deleting session".to_string())
    })?
}

#[cfg(target_os = "espidf")]
unsafe fn build_llm_config(
    config: &ClawAgentSystemConfig,
) -> Result<ClawApiConfig, CapabilityError> {
    let backend_type = required_string(config.backend_type)?
        .parse::<BackendKind>()
        .map_err(|_| CapabilityError::InvalidArg)?;

    let llm_config = ClawApiConfig::new(
        backend_type,
        required_string(config.api_key)?,
        required_string(config.model)?,
        required_string(config.base_url)?,
    );
    Ok(llm_config)
}

/// Copy a required, non-empty UTF-8 C string.
///
/// # Safety
/// `pointer` must be a valid NUL-terminated C string.
#[cfg(target_os = "espidf")]
unsafe fn required_string(pointer: *const c_char) -> Result<String, CapabilityError> {
    if pointer.is_null() {
        return Err(CapabilityError::InvalidArg);
    }
    let value = CStr::from_ptr(pointer)
        .to_str()
        .map(str::to_string)
        .map_err(|_| CapabilityError::InvalidArg)?;
    if value.is_empty() {
        return Err(CapabilityError::InvalidArg);
    }
    Ok(value)
}

/// Copy and parse a required session id.
///
/// # Safety
/// `pointer` must be a valid NUL-terminated UTF-8 `session-N` string.
#[cfg(target_os = "espidf")]
unsafe fn required_session_id(pointer: *const c_char) -> Result<SessionId, CapabilityError> {
    let value = required_string(pointer)?;
    if value == "default" {
        return Err(CapabilityError::InvalidArg);
    }
    SessionId::from_wire(&value).map_err(|_| CapabilityError::InvalidArg)
}

#[cfg(target_os = "espidf")]
fn validate_session_id_output_buffer(
    buffer: *mut c_char,
    capacity: usize,
    output_length: *mut usize,
) -> Result<(), CapabilityError> {
    if buffer.is_null() || output_length.is_null() {
        return Err(CapabilityError::InvalidArg);
    }
    if capacity < SESSION_ID_BUFFER_MIN_CAPACITY {
        return Err(CapabilityError::Failed(
            "session id buffer too small".to_string(),
        ));
    }
    Ok(())
}

/// Copy a Rust string into a writable C buffer and NUL-terminate it.
///
/// On overflow, copies the longest valid prefix, writes `*output_length` to the
/// required byte length, and returns `Failed`.
///
/// # Safety
/// `buffer` must be writable for `capacity` bytes and `output_length` must be a
/// valid out-pointer.
#[cfg(target_os = "espidf")]
unsafe fn copy_string_to_c_buffer(
    value: &str,
    buffer: *mut c_char,
    capacity: usize,
    output_length: *mut usize,
) -> Result<(), CapabilityError> {
    let output_length = output_length.as_mut().ok_or(CapabilityError::InvalidArg)?;
    *output_length = value.len();
    if buffer.is_null() || capacity == 0 {
        return Err(CapabilityError::InvalidArg);
    }

    let bytes = value.as_bytes();
    let max_payload = capacity.checked_sub(1).ok_or(CapabilityError::InvalidArg)?;
    let copied = bytes.len().min(max_payload);
    ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), copied);
    *buffer.add(copied) = 0;

    if copied != bytes.len() {
        return Err(CapabilityError::Failed(
            "output buffer too small for agent response".to_string(),
        ));
    }

    Ok(())
}

/// Register one capability into the registry.
///
/// # Safety
/// `registry` must be a handle from [`ClawCapabilityRegistry::into_raw`] and
/// `capability` a valid `ClawCapability` for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn claw_capability_register(
    registry: *mut ClawCapabilityRegistry,
    capability: *const ClawCapability,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { register_inner(registry, capability) }))
}

unsafe fn register_inner(
    registry: *mut ClawCapabilityRegistry,
    capability: *const ClawCapability,
) -> Result<(), CapabilityError> {
    let handle = registry.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let descriptor = capability.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let capability = unsafe { build_capability(descriptor)? };
    handle.registry.register(capability)
}

/// Register a group of capabilities sharing one optional lifecycle.
///
/// # Safety
/// `registry` must be a handle from [`ClawCapabilityRegistry::into_raw`] and
/// `group` a valid `ClawCapabilityGroup` for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn claw_capability_register_group(
    registry: *mut ClawCapabilityRegistry,
    group: *const ClawCapabilityGroup,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { register_group_inner(registry, group) }))
}

unsafe fn register_group_inner(
    registry: *mut ClawCapabilityRegistry,
    group: *const ClawCapabilityGroup,
) -> Result<(), CapabilityError> {
    let handle = registry.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let descriptor = group.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let group = unsafe { build_group(descriptor)? };
    handle.registry.register_group(group)
}

#[cfg(test)]
#[allow(
    clippy::unwrap_used,
    clippy::indexing_slicing,
    clippy::arithmetic_side_effects
)]
mod tests {
    use super::*;

    use core::ffi::{c_char, c_void};
    use core::ptr;
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::sync::Mutex;

    use claw_agent::{
        ChannelFuture, ChannelRuntime, InboundMessage, OutboundMessage, ToolInvocation,
    };

    fn ok_value() -> ClawCapabilityResult {
        ClawCapabilityResult {
            kind: ClawCapabilityErrorKind::Ok,
            message: ptr::null(),
        }
    }

    fn empty_lifecycle() -> ClawCapabilityLifecycle {
        ClawCapabilityLifecycle {
            init: None,
            start: None,
            stop: None,
            deinit: None,
        }
    }

    unsafe extern "C" fn echo_execute(
        _arguments_json: *const c_char,
        output_buffer: *mut c_char,
        output_capacity: usize,
        output_length: *mut usize,
        output_success: *mut bool,
        _user_context: *mut c_void,
    ) -> ClawCapabilityResult {
        let payload = b"echoed";
        let length = payload.len().min(output_capacity);
        ptr::copy_nonoverlapping(payload.as_ptr(), output_buffer.cast::<u8>(), length);
        *output_length = length;
        *output_success = true;
        ok_value()
    }

    unsafe extern "C" fn count_start(user_context: *mut c_void) -> ClawCapabilityResult {
        let counter = &*(user_context.cast::<AtomicU32>());
        counter.fetch_add(1, Ordering::SeqCst);
        ok_value()
    }

    fn tool_descriptor(
        id: *const c_char,
        schema: *const c_char,
        execute: Option<ClawCapabilityExecuteCallback>,
    ) -> ClawCapability {
        ClawCapability {
            id,
            description: ptr::null(),
            role: ClawCapabilityRole::Tool,
            role_data: ClawCapabilityRoleData {
                tool: ClawCapabilityTool {
                    schema_json: schema,
                    execute,
                },
            },
            lifecycle: empty_lifecycle(),
            user_context: ptr::null_mut(),
        }
    }

    #[test]
    fn register_tool_then_invoke() {
        let registry = Arc::new(Registry::new());
        let handle = ClawCapabilityRegistry::into_raw(Arc::clone(&registry));

        let descriptor = tool_descriptor(c"echo".as_ptr(), c"{}".as_ptr(), Some(echo_execute));
        let result = unsafe { claw_capability_register(handle, &descriptor) };
        assert_eq!(result.kind, ClawCapabilityErrorKind::Ok);

        let tools = registry.tools();
        assert_eq!(tools.len(), 1);
        let tool = tools.first().unwrap();
        assert_eq!(tool.name(), "echo");
        let output = tool
            .invoke(&ToolInvocation {
                id: None,
                name: "echo",
                arguments_json: "{}",
            })
            .unwrap();
        assert_eq!(output.output, "echoed");
        assert!(output.ok);

        unsafe { ClawCapabilityRegistry::drop_raw(handle) };
    }

    #[test]
    fn tool_without_execute_is_invalid_argument() {
        let registry = Arc::new(Registry::new());
        let handle = ClawCapabilityRegistry::into_raw(Arc::clone(&registry));

        let descriptor = tool_descriptor(c"broken".as_ptr(), c"{}".as_ptr(), None);
        let result = unsafe { claw_capability_register(handle, &descriptor) };
        assert_eq!(result.kind, ClawCapabilityErrorKind::InvalidArgument);
        assert!(registry.tools().is_empty());

        unsafe { ClawCapabilityRegistry::drop_raw(handle) };
    }

    #[test]
    fn null_registry_is_invalid_argument() {
        let descriptor = tool_descriptor(c"echo".as_ptr(), c"{}".as_ptr(), Some(echo_execute));
        let result = unsafe { claw_capability_register(ptr::null_mut(), &descriptor) };
        assert_eq!(result.kind, ClawCapabilityErrorKind::InvalidArgument);
    }

    #[test]
    fn duplicate_registration_is_already_exists() {
        let registry = Arc::new(Registry::new());
        let handle = ClawCapabilityRegistry::into_raw(Arc::clone(&registry));

        let descriptor = tool_descriptor(c"echo".as_ptr(), c"{}".as_ptr(), Some(echo_execute));
        assert_eq!(
            unsafe { claw_capability_register(handle, &descriptor) }.kind,
            ClawCapabilityErrorKind::Ok
        );
        assert_eq!(
            unsafe { claw_capability_register(handle, &descriptor) }.kind,
            ClawCapabilityErrorKind::AlreadyExists
        );

        unsafe { ClawCapabilityRegistry::drop_raw(handle) };
    }

    #[test]
    fn lifecycle_hook_runs_on_start_all() {
        let counter = Box::into_raw(Box::new(AtomicU32::new(0)));
        let registry = Arc::new(Registry::new());
        let handle = ClawCapabilityRegistry::into_raw(Arc::clone(&registry));

        let descriptor = ClawCapability {
            id: c"service".as_ptr(),
            description: ptr::null(),
            role: ClawCapabilityRole::None,
            role_data: ClawCapabilityRoleData {
                channel: ClawCapabilityChannel {
                    open: None,
                    close: None,
                    send: None,
                },
            },
            lifecycle: ClawCapabilityLifecycle {
                init: None,
                start: Some(count_start),
                stop: None,
                deinit: None,
            },
            user_context: counter.cast::<c_void>(),
        };
        assert_eq!(
            unsafe { claw_capability_register(handle, &descriptor) }.kind,
            ClawCapabilityErrorKind::Ok
        );

        registry.start_all().unwrap();
        assert_eq!(unsafe { (*counter).load(Ordering::SeqCst) }, 1);

        unsafe { ClawCapabilityRegistry::drop_raw(handle) };
        drop(unsafe { Box::from_raw(counter) });
    }

    #[derive(Default)]
    struct RecordingRuntime {
        messages: Mutex<Vec<InboundMessage>>,
    }

    impl ChannelRuntime for RecordingRuntime {
        fn push_message(&self, message: InboundMessage) -> ChannelFuture<'_> {
            self.messages.lock().unwrap().push(message);
            Box::pin(async { Ok(()) })
        }
    }

    #[test]
    fn channel_runtime_push_delivers_message() {
        let runtime = Arc::new(RecordingRuntime::default());
        let handle = ClawChannelRuntime::into_raw(Arc::clone(&runtime) as Arc<dyn ChannelRuntime>);

        let message = ClawInboundMessage {
            message_id: c"m1".as_ptr(),
            channel: c"local".as_ptr(),
            chat_id: c"chat".as_ptr(),
            sender_id: ptr::null(),
            text: c"hello".as_ptr(),
        };
        let result = unsafe { claw_channel_runtime_push(handle, &message) };
        assert_eq!(result.kind, ClawCapabilityErrorKind::Ok);

        let received = runtime.messages.lock().unwrap();
        assert_eq!(received.len(), 1);
        assert_eq!(received[0].text, "hello");
        assert_eq!(received[0].channel, "local");
        assert!(received[0].sender_id.is_none());

        drop(received);
        unsafe { ClawChannelRuntime::drop_raw(handle) };
    }

    #[test]
    fn channel_send_invokes_callback() {
        static SENT: Mutex<Vec<String>> = Mutex::new(Vec::new());

        unsafe extern "C" fn record_send(
            _channel: *const c_char,
            _chat_id: *const c_char,
            text: *const c_char,
            _reply_to_message_id: *const c_char,
            _user_context: *mut c_void,
        ) -> ClawCapabilityResult {
            let copied = core::ffi::CStr::from_ptr(text)
                .to_string_lossy()
                .into_owned();
            SENT.lock().unwrap().push(copied);
            ok_value()
        }
        unsafe extern "C" fn open_channel(
            runtime: *mut ClawChannelRuntime,
            _user_context: *mut c_void,
        ) -> ClawCapabilityResult {
            unsafe { ClawChannelRuntime::drop_raw(runtime) };
            ok_value()
        }
        unsafe extern "C" fn close_channel(_user_context: *mut c_void) -> ClawCapabilityResult {
            ok_value()
        }

        let registry = Arc::new(Registry::new());
        let handle = ClawCapabilityRegistry::into_raw(Arc::clone(&registry));

        let descriptor = ClawCapability {
            id: c"local".as_ptr(),
            description: ptr::null(),
            role: ClawCapabilityRole::Channel,
            role_data: ClawCapabilityRoleData {
                channel: ClawCapabilityChannel {
                    open: Some(open_channel),
                    close: Some(close_channel),
                    send: Some(record_send),
                },
            },
            lifecycle: empty_lifecycle(),
            user_context: ptr::null_mut(),
        };
        assert_eq!(
            unsafe { claw_capability_register(handle, &descriptor) }.kind,
            ClawCapabilityErrorKind::Ok
        );

        let channels = registry.channels();
        assert_eq!(channels.len(), 1);
        channels
            .first()
            .unwrap()
            .send(&OutboundMessage {
                channel: "local".to_string(),
                chat_id: "chat".to_string(),
                text: "reply".to_string(),
                reply_to_message_id: None,
            })
            .unwrap();
        assert_eq!(SENT.lock().unwrap().as_slice(), ["reply".to_string()]);

        unsafe { ClawCapabilityRegistry::drop_raw(handle) };
    }
}
