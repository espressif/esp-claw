use core::ffi::{c_char, CStr};
use core::pin::Pin;
use core::ptr;
use core::task::{Context, Poll};
use std::collections::HashMap;
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::str::FromStr;
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::{mpsc, Arc, Mutex, MutexGuard};
use std::task::Waker;
use std::time::Duration;

use claw_agent::{
    AgentError, AgentPersistenceConfig, AgentSystem, OpenSessionError, SessionControl,
    SessionControlError, SessionEvent, SessionEventStream, SessionId, StreamPart,
};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::{Cancel, ClawThread, ClawTimer, CoreAffinity, Priority};
use claw_log::{LevelFilter, LogOutput, TracingConfig};
use claw_sys::{EspIdfExecutor, EspIdfFs, EspIdfHttp, EspIdfThread, EspIdfTimer};

use futures_core::Stream;
use futures_lite::StreamExt;

use crate::abi::{
    ClawAgentConfig, ClawAgentEvent, EspErr, CLAW_AGENT_EVENT_KIND_CLOSED,
    CLAW_AGENT_EVENT_KIND_DONE, CLAW_AGENT_EVENT_KIND_ERROR, CLAW_AGENT_EVENT_KIND_OUTPUT,
    CLAW_AGENT_EVENT_KIND_OUTPUT_END, CLAW_AGENT_EVENT_KIND_REASONING,
    CLAW_AGENT_EVENT_KIND_REASONING_END, CLAW_AGENT_EVENT_KIND_TOOLS,
    CLAW_AGENT_EVENT_KIND_TOOLS_END, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE,
    ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, ESP_FAIL, ESP_OK,
};
use crate::tool::register_capability_tools;

/// The device agent runtime. `AgentSystem` is now backend-erased and
/// `Send + Sync` (its `Orchestrator` handle owns the drive worker), so it is held
/// directly here and driven concurrently: every open session has one event
/// stream while the FFI thread only enqueues commands and drains events.
type DeviceAgent = AgentSystem<EspIdfFs, EspIdfHttp, EspIdfTimer>;

const AGENT_BOOTSTRAP_STACK_SIZE: usize = 64 * 1024;

static RUNTIME: AtomicPtr<RuntimeController> = AtomicPtr::new(ptr::null_mut());
static RUNTIME_LOCK: Mutex<()> = Mutex::new(());

#[derive(Clone)]
struct RuntimeConfig {
    api: ClawApiConfig,
    persistence: AgentPersistenceConfig,
}

struct RuntimeController {
    config: RuntimeConfig,
    /// The running agent, present between `start` and `stop`/`deinit`. Dropping it
    /// joins the orchestrator's drive worker.
    agent: Option<DeviceAgent>,
    /// Open sessions keyed by numeric session id.
    sessions: Mutex<HashMap<u32, Arc<OpenSession>>>,
}

/// One open session connection. The stream is drained incrementally — one
/// [`SessionEvent`] per `receive` — while commands use the cloneable control half.
struct OpenSession {
    stream: Mutex<SessionEventStream>,
    control: SessionControl,
    terminal: AtomicBool,
}

/// One event handed across the FFI, mapped from a [`SessionEvent`].
enum FfiEvent {
    Output(String),
    OutputEnd,
    Reasoning(String),
    ReasoningEnd,
    Tools(String),
    ToolsEnd,
    Done,
    Error(String),
    Closed,
}

impl FfiEvent {
    fn is_terminal(&self) -> bool {
        matches!(self, FfiEvent::Closed)
    }
}

/// Map a stream event to its FFI form. `None` marks a bracket/meta event
/// (`TurnStarted`, `IterationStarted`/`IterationEnded`) that C does not surface;
/// the puller skips it and pulls the next.
fn map_event(event: SessionEvent) -> Option<FfiEvent> {
    match event {
        SessionEvent::Output(StreamPart::Delta(text)) => Some(FfiEvent::Output(text)),
        SessionEvent::Output(StreamPart::End) => Some(FfiEvent::OutputEnd),
        SessionEvent::Reasoning(StreamPart::Delta(text)) => Some(FfiEvent::Reasoning(text)),
        SessionEvent::Reasoning(StreamPart::End) => Some(FfiEvent::ReasoningEnd),
        SessionEvent::ToolCalls(StreamPart::Delta(call)) => Some(FfiEvent::Tools(call.name)),
        SessionEvent::ToolCalls(StreamPart::End) => Some(FfiEvent::ToolsEnd),
        SessionEvent::Error { message } => Some(FfiEvent::Error(message)),
        SessionEvent::TurnEnded { .. } => Some(FfiEvent::Done),
        SessionEvent::Closed => Some(FfiEvent::Closed),
        SessionEvent::TurnStarted { .. }
        | SessionEvent::IterationStarted { .. }
        | SessionEvent::IterationEnded => None,
    }
}

#[no_mangle]
/// Initialize the C agent runtime.
///
/// # Safety
/// `config` must point to valid UTF-8 C strings for this call.
pub unsafe extern "C" fn claw_agent_init(config: *const ClawAgentConfig) -> EspErr {
    ffi_result(|| init(config))
}

#[no_mangle]
pub extern "C" fn claw_agent_start() -> EspErr {
    ffi_result(start)
}

#[no_mangle]
pub extern "C" fn claw_agent_stop() -> EspErr {
    ffi_result(stop)
}

#[no_mangle]
pub extern "C" fn claw_agent_deinit() -> EspErr {
    ffi_result(deinit)
}

#[no_mangle]
/// Submit one inbound message to an explicit numeric session.
///
/// # Safety
/// `input` must point to valid UTF-8 C strings for this call.
pub unsafe extern "C" fn claw_agent_session_submit(session_id: u32, text: *const c_char) -> EspErr {
    ffi_result(|| submit_session(session_id, text))
}

#[no_mangle]
/// Open a numeric session's event stream.
pub extern "C" fn claw_agent_session_open(session_id: u32) -> EspErr {
    ffi_result(|| session_open(session_id))
}

#[no_mangle]
/// Create a new numeric session.
///
/// # Safety
/// `out_session_id` must point to writable memory for one u32.
pub unsafe extern "C" fn claw_agent_session_create(out_session_id: *mut u32) -> EspErr {
    ffi_result(|| session_create(out_session_id))
}

#[no_mangle]
/// List live numeric sessions.
///
/// # Safety
/// `out_count` must point to writable memory for one usize. `out_session_ids`
/// must be writable for `capacity` u32 values unless `capacity` is zero.
pub unsafe extern "C" fn claw_agent_session_list(
    out_session_ids: *mut u32,
    capacity: usize,
    out_count: *mut usize,
) -> EspErr {
    ffi_result(|| session_list(out_session_ids, capacity, out_count))
}

#[no_mangle]
/// Close a numeric session stream.
pub extern "C" fn claw_agent_session_close(session_id: u32) -> EspErr {
    ffi_result(|| session_close(session_id))
}

#[no_mangle]
/// Delete a numeric session id.
pub extern "C" fn claw_agent_session_delete(session_id: u32) -> EspErr {
    ffi_result(|| session_delete(session_id))
}

#[no_mangle]
/// Receive the next event from an open session (one event per call).
///
/// # Safety
/// `out_event` must point to writable memory for one event.
pub unsafe extern "C" fn claw_agent_session_receive(
    session_id: u32,
    out_event: *mut ClawAgentEvent,
    timeout_ms: u32,
) -> EspErr {
    ffi_result(|| receive(session_id, out_event, timeout_ms))
}

#[no_mangle]
/// Request graceful interruption of the active foreground turn.
pub extern "C" fn claw_agent_session_interrupt(session_id: u32) -> EspErr {
    ffi_result(|| session_interrupt(session_id))
}

#[no_mangle]
/// Request hard cancellation of foreground and background session work.
pub extern "C" fn claw_agent_session_cancel(session_id: u32) -> EspErr {
    ffi_result(|| session_cancel(session_id))
}

#[no_mangle]
/// Release strings owned by an event returned from `claw_agent_session_receive`.
///
/// # Safety
/// `event` must be null or an event returned by `claw_agent_session_receive`.
pub unsafe extern "C" fn claw_agent_event_free(event: *mut ClawAgentEvent) {
    free_event(event);
}

fn init(config: *const ClawAgentConfig) -> Result<(), CabiError> {
    let _ = claw_log::init_logger(LevelFilter::Info, LogOutput::Stderr);
    let _ = claw_log::init_tracing(
        TracingConfig::default()
            .with_context_group_keys("run", ["session", "turn", "agent", "iteration"]),
    );

    let config = unsafe { config.as_ref() }.ok_or(CabiError::InvalidArgument)?;
    let backend_type = required_string(config.backend_type)?;
    let backend = BackendKind::from_str(&backend_type).map_err(|_| CabiError::InvalidArgument)?;
    let api = ClawApiConfig::new(
        backend,
        required_string(config.api_key)?,
        required_string(config.model)?,
        required_string(config.base_url)?,
    );
    let persistence_dir = required_string(config.persistence_dir)?;
    // Skill roots are scanned in priority order: writable DATA skills first, then
    // read-only firmware skills. Both are optional; a missing/blank root is simply
    // skipped so the agent still starts (with fewer skills).
    let mut skill_roots = Vec::new();
    for root in [
        optional_string(config.skills_root_dir)?,
        optional_string(config.system_skills_root_dir)?,
    ]
    .into_iter()
    .flatten()
    {
        if !root.trim().is_empty() {
            skill_roots.push(root);
        }
    }
    let persistence = AgentPersistenceConfig {
        persistence_root: persistence_dir,
        skill_roots,
    };

    let _guard = lock_runtime();
    if !RUNTIME.load(Ordering::Acquire).is_null() {
        return Err(CabiError::InvalidState);
    }

    let runtime = Box::new(RuntimeController {
        config: RuntimeConfig { api, persistence },
        agent: None,
        sessions: Mutex::new(HashMap::new()),
    });
    RUNTIME.store(Box::into_raw(runtime), Ordering::Release);
    Ok(())
}

fn start() -> Result<(), CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    if runtime.agent.is_some() {
        return Ok(());
    }

    // Capability registration checkpoints every inserted tool. Keep that deep
    // serde/filesystem call chain off ESP-IDF's small `main` task stack.
    let api = runtime.config.api.clone();
    let persistence = runtime.config.persistence.clone();
    let (result_tx, result_rx) = mpsc::sync_channel(1);
    let worker = EspIdfThread::spawn_worker(
        "claw_bootstrap",
        AGENT_BOOTSTRAP_STACK_SIZE,
        Priority::Normal,
        CoreAffinity::Any,
        move || {
            let result = build_agent(api, persistence);
            let _ = result_tx.send(result);
        },
    )
    .map_err(CabiError::BootstrapSpawn)?;

    let result = result_rx.recv();
    worker.join();
    let agent = result.map_err(|_| CabiError::BootstrapExited)??;
    runtime.agent = Some(agent);
    Ok(())
}

fn build_agent(
    api: ClawApiConfig,
    persistence: AgentPersistenceConfig,
) -> Result<DeviceAgent, CabiError> {
    let agent = DeviceAgent::new::<EspIdfThread, EspIdfExecutor>(api, persistence)?;
    register_capability_tools(agent.tool_registry())?;
    agent.start_all()?;
    Ok(agent)
}

fn stop() -> Result<(), CabiError> {
    // Take the agent out under the lock, then stop/drop it outside so the
    // orchestrator worker join never happens while holding the runtime lock.
    let agent = {
        let _guard = lock_runtime();
        let runtime = runtime_mut()?;
        open_sessions(runtime).clear();
        runtime.agent.take()
    };
    if let Some(agent) = agent {
        let result = agent.stop_all().map_err(CabiError::from);
        drop(agent);
        return result.map(|_| ());
    }
    Ok(())
}

fn deinit() -> Result<(), CabiError> {
    let ptr = {
        let _guard = lock_runtime();
        RUNTIME.swap(ptr::null_mut(), Ordering::AcqRel)
    };
    if ptr.is_null() {
        return Ok(());
    }

    let mut runtime = unsafe { Box::from_raw(ptr) };
    let agent = runtime.agent.take();
    drop(runtime);
    if let Some(agent) = agent {
        let _ = agent.stop_all();
        drop(agent);
    }
    Ok(())
}

fn submit_session(session_id: u32, text: *const c_char) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let text = required_string(text)?;
    let session = {
        let _guard = lock_runtime();
        let runtime = runtime_mut()?;
        runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
        get_open_session_locked(runtime, session_id)?.ok_or(CabiError::NotFound)?
    };
    futures_lite::future::block_on(session.control.submit(text))
        .map_err(|_| CabiError::InvalidState)
}

fn session_open(session_id: u32) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let agent = runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
    if open_sessions(runtime).contains_key(&session_id) {
        return Err(CabiError::InvalidState);
    }
    let (control, stream) = agent
        .open_session(SessionId::new(session_id))
        .map_err(open_session_error)?;
    open_sessions(runtime).insert(
        session_id,
        Arc::new(OpenSession {
            stream: Mutex::new(stream),
            control,
            terminal: AtomicBool::new(false),
        }),
    );
    Ok(())
}

fn session_create(out_session_id: *mut u32) -> Result<(), CabiError> {
    let out_session_id = unsafe { out_session_id.as_mut() }.ok_or(CabiError::InvalidArgument)?;
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let agent = runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
    *out_session_id = agent.new_session().0;
    Ok(())
}

fn session_list(
    out_session_ids: *mut u32,
    capacity: usize,
    out_count: *mut usize,
) -> Result<(), CabiError> {
    let out_count = unsafe { out_count.as_mut() }.ok_or(CabiError::InvalidArgument)?;
    if capacity > 0 && out_session_ids.is_null() {
        return Err(CabiError::InvalidArgument);
    }

    let sessions = {
        let _guard = lock_runtime();
        let runtime = runtime_mut()?;
        let agent = runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
        agent.list_sessions()
    };
    *out_count = sessions.len();
    if capacity < sessions.len() {
        return Err(CabiError::InvalidSize);
    }
    if capacity == 0 {
        return Ok(());
    }

    let out_session_ids = unsafe { core::slice::from_raw_parts_mut(out_session_ids, capacity) };
    for (slot, session) in out_session_ids.iter_mut().zip(sessions) {
        *slot = session.0;
    }
    Ok(())
}

fn session_close(session_id: u32) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let session = get_open_session(session_id)?.ok_or(CabiError::NotFound)?;
    futures_lite::future::block_on(session.control.close_session()).map_err(session_control_error)
}

fn session_delete(session_id: u32) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let agent = runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
    agent
        .delete_session(SessionId::new(session_id))
        .map_err(session_control_error)
}

fn receive(
    session_id: u32,
    out_event: *mut ClawAgentEvent,
    timeout_ms: u32,
) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let out_event = unsafe { out_event.as_mut() }.ok_or(CabiError::InvalidArgument)?;

    let Some(session) = get_open_session(session_id)? else {
        return Err(CabiError::NotFound);
    };
    if session.terminal.load(Ordering::Acquire) {
        return Err(CabiError::Timeout);
    }

    let next = {
        let mut stream = session
            .stream
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        if session.terminal.load(Ordering::Acquire) {
            return Err(CabiError::Timeout);
        }
        if timeout_ms == 0 {
            next_ready(&mut stream)
        } else {
            next_within(&mut stream, timeout_ms)
        }
    };

    match next {
        Some(event) => {
            let terminal = event.is_terminal();
            write_event(out_event, event)?;
            if terminal {
                session.terminal.store(true, Ordering::Release);
                remove_open_session(session_id, &session);
            }
            Ok(())
        }
        None => Err(CabiError::Timeout),
    }
}

fn session_interrupt(session_id: u32) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let session = get_open_session(session_id)?.ok_or(CabiError::NotFound)?;
    if session.terminal.load(Ordering::Acquire) {
        return Err(CabiError::NotFound);
    }
    futures_lite::future::block_on(session.control.interrupt()).map_err(|_| CabiError::InvalidState)
}

fn session_cancel(session_id: u32) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let session = get_open_session(session_id)?.ok_or(CabiError::NotFound)?;
    if session.terminal.load(Ordering::Acquire) {
        return Err(CabiError::NotFound);
    }
    futures_lite::future::block_on(session.control.cancel()).map_err(|_| CabiError::InvalidState)
}

/// Pull the next surfaced event already buffered on the stream without blocking.
/// Returns `None` when nothing is ready yet; skips bracket/meta events; maps a
/// closed stream to [`FfiEvent::Closed`].
fn next_ready(stream: &mut SessionEventStream) -> Option<FfiEvent> {
    let mut context = Context::from_waker(Waker::noop());
    loop {
        match Pin::new(&mut *stream).poll_next(&mut context) {
            Poll::Ready(Some(event)) => {
                if let Some(mapped) = map_event(event) {
                    return Some(mapped);
                }
            }
            Poll::Ready(None) => return Some(FfiEvent::Closed),
            Poll::Pending => return None,
        }
    }
}

/// Pull the next surfaced event, waiting up to `timeout_ms`. Returns `None` on
/// timeout (the stream is retained for a later `receive`); skips bracket/meta
/// events; maps a closed stream to [`FfiEvent::Closed`].
fn next_within(stream: &mut SessionEventStream, timeout_ms: u32) -> Option<FfiEvent> {
    let abort = AtomicBool::new(false);
    let mut timer = EspIdfTimer;
    futures_lite::future::block_on(async {
        let pull = async {
            loop {
                match stream.next().await {
                    Some(event) => {
                        if let Some(mapped) = map_event(event) {
                            return Some(mapped);
                        }
                    }
                    None => return Some(FfiEvent::Closed),
                }
            }
        };
        let timeout = async {
            let _ = timer
                .sleep(
                    Duration::from_millis(u64::from(timeout_ms)),
                    Cancel::new(&abort),
                )
                .await;
            None::<FfiEvent>
        };
        futures_lite::future::or(pull, timeout).await
    })
}

fn get_open_session(session_id: u32) -> Result<Option<Arc<OpenSession>>, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    get_open_session_locked(runtime, session_id)
}

fn get_open_session_locked(
    runtime: &RuntimeController,
    session_id: u32,
) -> Result<Option<Arc<OpenSession>>, CabiError> {
    Ok(open_sessions(runtime).get(&session_id).cloned())
}

fn remove_open_session(session_id: u32, session: &Arc<OpenSession>) {
    let _guard = lock_runtime();
    if let Ok(runtime) = runtime_mut() {
        let mut sessions = open_sessions(runtime);
        if sessions
            .get(&session_id)
            .is_some_and(|stored| Arc::ptr_eq(stored, session))
        {
            sessions.remove(&session_id);
        }
    }
}

fn open_sessions(runtime: &RuntimeController) -> MutexGuard<'_, HashMap<u32, Arc<OpenSession>>> {
    runtime
        .sessions
        .lock()
        .unwrap_or_else(|poison| poison.into_inner())
}

fn runtime_mut() -> Result<&'static mut RuntimeController, CabiError> {
    let ptr = RUNTIME.load(Ordering::Acquire);
    if ptr.is_null() {
        return Err(CabiError::InvalidState);
    }
    Ok(unsafe { &mut *ptr })
}

/// Marshal one [`FfiEvent`] into the C event struct: content events fill `text`,
/// an error fills `error_message`, and the rest stay null.
fn write_event(out_event: &mut ClawAgentEvent, event: FfiEvent) -> Result<(), CabiError> {
    let (kind, text, error_message) = match event {
        FfiEvent::Output(text) => (CLAW_AGENT_EVENT_KIND_OUTPUT, Some(text), None),
        FfiEvent::OutputEnd => (CLAW_AGENT_EVENT_KIND_OUTPUT_END, None, None),
        FfiEvent::Reasoning(text) => (CLAW_AGENT_EVENT_KIND_REASONING, Some(text), None),
        FfiEvent::ReasoningEnd => (CLAW_AGENT_EVENT_KIND_REASONING_END, None, None),
        FfiEvent::Tools(text) => (CLAW_AGENT_EVENT_KIND_TOOLS, Some(text), None),
        FfiEvent::ToolsEnd => (CLAW_AGENT_EVENT_KIND_TOOLS_END, None, None),
        FfiEvent::Done => (CLAW_AGENT_EVENT_KIND_DONE, None, None),
        FfiEvent::Error(message) => (CLAW_AGENT_EVENT_KIND_ERROR, None, Some(message)),
        FfiEvent::Closed => (CLAW_AGENT_EVENT_KIND_CLOSED, None, None),
    };
    let text = match text {
        Some(value) => cstring_raw(&value)?,
        None => ptr::null_mut(),
    };
    let error_message = match error_message {
        Some(value) => cstring_raw(&value)?,
        None => ptr::null_mut(),
    };
    *out_event = ClawAgentEvent {
        kind,
        text,
        error_message,
    };
    Ok(())
}

fn free_event(event: *mut ClawAgentEvent) {
    let Some(event) = (unsafe { event.as_mut() }) else {
        return;
    };
    free_cstring(event.text);
    free_cstring(event.error_message);
    event.text = ptr::null_mut();
    event.error_message = ptr::null_mut();
}

fn cstring_raw(value: &str) -> Result<*mut c_char, CabiError> {
    let sanitized = value.replace('\0', "\\0");
    CString::new(sanitized)
        .map(CString::into_raw)
        .map_err(|_| CabiError::InvalidArgument)
}

fn free_cstring(value: *mut c_char) {
    if value.is_null() {
        return;
    }
    let _ = unsafe { CString::from_raw(value) };
}

fn required_string(ptr: *const c_char) -> Result<String, CabiError> {
    optional_string(ptr)?.ok_or(CabiError::InvalidArgument)
}

fn optional_string(ptr: *const c_char) -> Result<Option<String>, CabiError> {
    if ptr.is_null() {
        return Ok(None);
    }
    let text = unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|_| CabiError::InvalidArgument)?;
    Ok(Some(text.to_owned()))
}

fn open_session_error(error: AgentError) -> CabiError {
    match error {
        AgentError::OpenSession(OpenSessionError::SessionNotFound(_)) => CabiError::NotFound,
        AgentError::OpenSession(OpenSessionError::AlreadyOpen(_)) => CabiError::InvalidState,
        AgentError::OpenSession(OpenSessionError::WorkerStopped) => CabiError::InvalidState,
        other => CabiError::Agent(other),
    }
}

fn session_control_error(error: SessionControlError) -> CabiError {
    match error {
        SessionControlError::SessionClosed(_) => CabiError::NotFound,
        SessionControlError::Busy(_) | SessionControlError::WorkerStopped => {
            CabiError::InvalidState
        }
    }
}

fn lock_runtime() -> MutexGuard<'static, ()> {
    RUNTIME_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn ffi_result(op: impl FnOnce() -> Result<(), CabiError>) -> EspErr {
    match catch_unwind(AssertUnwindSafe(op)) {
        Ok(Ok(())) => ESP_OK,
        Ok(Err(error)) => {
            let esp_err = error.esp_err();
            if error.should_log() {
                tracing::error!(target: "claw_cabi", error = %error, "C ABI call failed");
            }
            esp_err
        }
        Err(_) => {
            tracing::error!(target: "claw_cabi", "C ABI call panicked");
            ESP_FAIL
        }
    }
}

#[derive(Debug, thiserror::Error)]
enum CabiError {
    #[error("invalid argument")]
    InvalidArgument,
    #[error("invalid state")]
    InvalidState,
    #[error("invalid size")]
    InvalidSize,
    #[error("not found")]
    NotFound,
    #[error("timeout")]
    Timeout,
    #[error("failed to spawn agent bootstrap worker: {0}")]
    BootstrapSpawn(#[source] std::io::Error),
    #[error("agent bootstrap worker exited before returning a result")]
    BootstrapExited,
    #[error(transparent)]
    Agent(#[from] AgentError),
    #[error(transparent)]
    Tool(#[from] claw_tool::ToolInvokeError),
    #[error(transparent)]
    CapTool(#[from] crate::tool::CapToolError),
}

impl CabiError {
    fn should_log(&self) -> bool {
        matches!(
            self,
            Self::BootstrapSpawn(_)
                | Self::BootstrapExited
                | Self::Agent(_)
                | Self::Tool(_)
                | Self::CapTool(_)
        )
    }

    fn esp_err(&self) -> EspErr {
        match self {
            Self::InvalidArgument => ESP_ERR_INVALID_ARG,
            Self::InvalidState => ESP_ERR_INVALID_STATE,
            Self::InvalidSize => ESP_ERR_INVALID_SIZE,
            Self::NotFound => ESP_ERR_NOT_FOUND,
            Self::Timeout => ESP_ERR_TIMEOUT,
            Self::BootstrapSpawn(_)
            | Self::BootstrapExited
            | Self::Agent(_)
            | Self::Tool(_)
            | Self::CapTool(_) => ESP_FAIL,
        }
    }
}
