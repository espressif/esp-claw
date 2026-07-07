use core::ffi::{c_char, CStr};
use core::pin::Pin;
use core::ptr;
use core::task::{Context, Poll};
use std::collections::HashMap;
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::str::FromStr;
use std::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::task::{Wake, Waker};
use std::time::Duration;

use claw_agent::{
    AgentError, AgentEvent, AgentEventStream as SubmitStream, AgentPersistenceConfig, AgentSystem,
    DeliveryKind, SessionId,
};
use claw_api::{BackendKind, ClawApiConfig};
use claw_interface::{Cancel, ClawTimer};
use claw_sys::{EspIdfExecutor, EspIdfFs, EspIdfHttp, EspIdfThread, EspIdfTimer};

use futures_core::Stream;
use futures_lite::StreamExt;

use crate::abi::{
    ClawAgentConfig, ClawAgentEvent, EspErr, CLAW_AGENT_EVENT_KIND_DONE,
    CLAW_AGENT_EVENT_KIND_ERROR, CLAW_AGENT_EVENT_KIND_OUTPUT, CLAW_AGENT_EVENT_KIND_REASONING,
    CLAW_AGENT_EVENT_KIND_TOOLS, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE, ESP_ERR_INVALID_STATE,
    ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, ESP_FAIL, ESP_OK,
};
use crate::tool::{register_capability_tools, CapabilityContextData};

/// The device agent runtime. `AgentSystem` is now backend-erased and
/// `Send + Sync` (its `Orchestrator` handle owns the drive worker), so it is held
/// directly here and driven concurrently: every `submit` runs on the
/// orchestrator's own worker thread while the FFI thread only enqueues and later
/// drains the resulting event stream.
type DeviceAgent = AgentSystem<EspIdfFs, EspIdfHttp, EspIdfTimer>;

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
    /// In-flight submissions keyed by request id: each holds its remaining event
    /// stream, pulled one event at a time (lazily, with a timeout) by [`receive`].
    pending: Mutex<HashMap<u32, PendingSubmit>>,
    next_request_id: AtomicU32,
}

/// One in-flight submission: the session it belongs to and its remaining event
/// stream. The stream is drained incrementally — one [`AgentEvent`] per
/// `receive` — and the entry is removed once a terminal event (done/error) is
/// delivered.
struct PendingSubmit {
    session_id: u32,
    stream: SubmitStream,
}

/// One event handed across the FFI, mapped from an [`AgentEvent`].
///
/// [`Done`](Self::Done) and [`Error`](Self::Error) are terminal: after either,
/// the submission is dropped from `pending`.
enum FfiEvent {
    Output(String),
    Reasoning(String),
    Tools(String),
    Done,
    Error(String),
}

impl FfiEvent {
    fn is_terminal(&self) -> bool {
        matches!(self, FfiEvent::Done | FfiEvent::Error(_))
    }
}

/// Map a stream event to its FFI form. `None` marks a bracket/meta event
/// (`TurnStarted`, `IterationStarted`/`IterationEnded`) that C does not surface;
/// the puller skips it and pulls the next.
fn map_event(event: AgentEvent) -> Option<FfiEvent> {
    match event {
        AgentEvent::Output { text } => Some(FfiEvent::Output(text)),
        AgentEvent::Reasoning { text } => Some(FfiEvent::Reasoning(text)),
        AgentEvent::Tools { names } => Some(FfiEvent::Tools(names.join(", "))),
        AgentEvent::Error { message } => Some(FfiEvent::Error(message)),
        AgentEvent::TurnEnded => Some(FfiEvent::Done),
        AgentEvent::TurnStarted
        | AgentEvent::IterationStarted { .. }
        | AgentEvent::IterationEnded => None,
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
pub unsafe extern "C" fn claw_agent_session_submit(
    session_id: u32,
    text: *const c_char,
    out_request_id: *mut u32,
) -> EspErr {
    ffi_result(|| submit_session(session_id, text, out_request_id))
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
/// Delete a numeric session.
pub extern "C" fn claw_agent_session_delete(session_id: u32) -> EspErr {
    ffi_result(|| session_delete(session_id))
}

#[no_mangle]
/// Receive the next event of a submitted turn (one event per call).
///
/// # Safety
/// `out_event` must point to writable memory for one event.
pub unsafe extern "C" fn claw_agent_session_receive(
    session_id: u32,
    request_id: u32,
    out_event: *mut ClawAgentEvent,
    timeout_ms: u32,
) -> EspErr {
    ffi_result(|| receive(session_id, request_id, out_event, timeout_ms))
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
    let persistence = AgentPersistenceConfig::new(&persistence_dir).with_skill_roots(skill_roots);

    let _guard = lock_runtime();
    if !RUNTIME.load(Ordering::Acquire).is_null() {
        return Err(CabiError::InvalidState);
    }

    let runtime = Box::new(RuntimeController {
        config: RuntimeConfig { api, persistence },
        agent: None,
        pending: Mutex::new(HashMap::new()),
        next_request_id: AtomicU32::new(1),
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
    // `AgentSystem::new` spawns the orchestrator's drive worker (via `EspIdfThread`)
    // and blocks until it reports readiness, so a build failure surfaces here.
    let agent =
        AgentSystem::<EspIdfFs, EspIdfHttp, EspIdfTimer>::new::<EspIdfThread, EspIdfExecutor>(
            runtime.config.api.clone(),
            runtime.config.persistence.clone(),
        )?;
    register_capability_tools(agent.tool_registry())?;
    agent.start_all()?;
    runtime.agent = Some(agent);
    Ok(())
}

fn stop() -> Result<(), CabiError> {
    // Take the agent out under the lock, then stop/drop it outside so the
    // orchestrator worker join never happens while holding the runtime lock.
    let agent = {
        let _guard = lock_runtime();
        let runtime = runtime_mut()?;
        pending_map(runtime).clear();
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

fn submit_session(
    session_id: u32,
    text: *const c_char,
    out_request_id: *mut u32,
) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let request_id = submit_owned(
        SessionId::new(session_id),
        required_string(text)?,
        !out_request_id.is_null(),
    )?;
    if let Some(out_request_id) = unsafe { out_request_id.as_mut() } {
        *out_request_id = request_id;
    }
    Ok(())
}

fn submit_owned(session: SessionId, text: String, store_response: bool) -> Result<u32, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let agent = runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
    if !agent.list_sessions().contains(&session) {
        return Err(CabiError::NotFound);
    }
    let request_id = runtime.next_request_id.fetch_add(1, Ordering::AcqRel);
    if request_id == 0 {
        return Err(CabiError::InvalidState);
    }
    // The per-submission capability context rides through the core drive as a
    // type-erased `SharedContext`; the engine installs it around this turn so a
    // `CapTool` deep in the drive reads it back via `claw_tool::current_context`.
    let context: claw_tool::SharedContext = Arc::new(CapabilityContextData {
        request_id,
        ..CapabilityContextData::default()
    });
    let stream = agent.submit_with_context(session, text, DeliveryKind::Interrupt, Some(context));
    if store_response {
        pending_map(runtime).insert(
            request_id,
            PendingSubmit {
                session_id: session.0,
                stream,
            },
        );
    }
    // When no response is wanted, drop the stream: the orchestrator worker still
    // drives the turn to completion; the closed channel just discards its events.
    Ok(request_id)
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

fn session_delete(session_id: u32) -> Result<(), CabiError> {
    if session_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let agent = runtime.agent.as_ref().ok_or(CabiError::InvalidState)?;
    agent
        .delete_session(SessionId::new(session_id))
        .map_err(|_| CabiError::NotFound)
}

fn receive(
    session_id: u32,
    request_id: u32,
    out_event: *mut ClawAgentEvent,
    timeout_ms: u32,
) -> Result<(), CabiError> {
    if session_id == 0 || request_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let out_event = unsafe { out_event.as_mut() }.ok_or(CabiError::InvalidArgument)?;

    // Check the submission out so the (possibly long) wait does not hold the
    // runtime lock; reinsert it unless we just delivered a terminal event.
    let Some(mut pending) = take_pending(request_id)? else {
        // Unknown or already-consumed request id: nothing to deliver.
        return Err(CabiError::Timeout);
    };
    if pending.session_id != session_id {
        reinsert_pending(request_id, pending);
        return Err(CabiError::InvalidArgument);
    }

    let next = if timeout_ms == 0 {
        next_ready(&mut pending.stream)
    } else {
        next_within(&mut pending.stream, timeout_ms)
    };

    match next {
        Some(event) => {
            let terminal = event.is_terminal();
            write_event(out_event, event)?;
            // Keep the stream alive for the next pull unless this was the last
            // event; a terminal event drops `pending` so the id is consumed.
            if !terminal {
                reinsert_pending(request_id, pending);
            }
            Ok(())
        }
        None => {
            reinsert_pending(request_id, pending);
            Err(CabiError::Timeout)
        }
    }
}

/// Pull the next surfaced event already buffered on the stream without blocking.
/// Returns `None` when nothing is ready yet; skips bracket/meta events; maps a
/// closed stream to [`FfiEvent::Done`].
fn next_ready(stream: &mut SubmitStream) -> Option<FfiEvent> {
    let waker = Waker::from(Arc::new(NoopWake));
    let mut context = Context::from_waker(&waker);
    loop {
        match Pin::new(&mut *stream).poll_next(&mut context) {
            Poll::Ready(Some(event)) => {
                if let Some(mapped) = map_event(event) {
                    return Some(mapped);
                }
            }
            Poll::Ready(None) => return Some(FfiEvent::Done),
            Poll::Pending => return None,
        }
    }
}

/// Pull the next surfaced event, waiting up to `timeout_ms`. Returns `None` on
/// timeout (the stream is retained for a later `receive`); skips bracket/meta
/// events; maps a closed stream to [`FfiEvent::Done`].
fn next_within(stream: &mut SubmitStream, timeout_ms: u32) -> Option<FfiEvent> {
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
                    None => return Some(FfiEvent::Done),
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

struct NoopWake;

impl Wake for NoopWake {
    fn wake(self: Arc<Self>) {}
}

fn take_pending(request_id: u32) -> Result<Option<PendingSubmit>, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    Ok(pending_map(runtime).remove(&request_id))
}

fn reinsert_pending(request_id: u32, pending: PendingSubmit) {
    let _guard = lock_runtime();
    if let Ok(runtime) = runtime_mut() {
        pending_map(runtime).insert(request_id, pending);
    }
}

fn pending_map(runtime: &RuntimeController) -> MutexGuard<'_, HashMap<u32, PendingSubmit>> {
    runtime
        .pending
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
        FfiEvent::Reasoning(text) => (CLAW_AGENT_EVENT_KIND_REASONING, Some(text), None),
        FfiEvent::Tools(text) => (CLAW_AGENT_EVENT_KIND_TOOLS, Some(text), None),
        FfiEvent::Done => (CLAW_AGENT_EVENT_KIND_DONE, None, None),
        FfiEvent::Error(message) => (CLAW_AGENT_EVENT_KIND_ERROR, None, Some(message)),
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

fn lock_runtime() -> MutexGuard<'static, ()> {
    RUNTIME_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn ffi_result(op: impl FnOnce() -> Result<(), CabiError>) -> EspErr {
    match catch_unwind(AssertUnwindSafe(op)) {
        Ok(Ok(())) => ESP_OK,
        Ok(Err(error)) => error.esp_err(),
        Err(_) => ESP_FAIL,
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
    #[error(transparent)]
    Agent(#[from] AgentError),
    #[error(transparent)]
    Tool(#[from] claw_tool::ToolInvokeError),
    #[error(transparent)]
    CapTool(#[from] crate::tool::CapToolError),
}

impl CabiError {
    fn esp_err(&self) -> EspErr {
        match self {
            Self::InvalidArgument => ESP_ERR_INVALID_ARG,
            Self::InvalidState => ESP_ERR_INVALID_STATE,
            Self::InvalidSize => ESP_ERR_INVALID_SIZE,
            Self::NotFound => ESP_ERR_NOT_FOUND,
            Self::Timeout => ESP_ERR_TIMEOUT,
            Self::Agent(_) | Self::Tool(_) | Self::CapTool(_) => ESP_FAIL,
        }
    }
}
