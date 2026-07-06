use core::ffi::{c_char, CStr};
use core::future::Future;
use core::pin::Pin;
use core::ptr;
use core::task::{Context, Poll};
use std::collections::{HashMap, VecDeque};
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::str::FromStr;
use std::sync::atomic::{AtomicPtr, AtomicU32, Ordering};
use std::sync::{mpsc, Arc, Condvar, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use claw_agent::{AgentError, AgentPersistenceConfig, AgentSystem};
use claw_api::{BackendKind, ClawApiConfig};
use claw_core::{DeliveryKind, DriveOutput, SessionId};
use claw_interface::{ClawThread, CoreAffinity, Priority, WorkerHandle};
use claw_sys::{EspIdfFs, EspIdfHttp, EspIdfThread, EspIdfTimer};

use async_channel::{Receiver, Sender};
use futures_core::Stream;

use crate::abi::{
    ClawAgentConfig, ClawAgentResponse, EspErr, CLAW_AGENT_RESPONSE_STATUS_ERROR,
    CLAW_AGENT_RESPONSE_STATUS_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_SIZE,
    ESP_ERR_INVALID_STATE, ESP_ERR_NOT_FOUND, ESP_ERR_TIMEOUT, ESP_FAIL, ESP_OK,
};
use crate::executor;
use crate::tool::{register_capability_tools, with_capability_context, CapabilityContextData};

type DeviceAgent = Rc<AgentSystem<EspIdfFs, EspIdfHttp, EspIdfTimer>>;
type InflightSubmit = Pin<Box<dyn Future<Output = CompletedSubmit>>>;

const AGENT_WORKER_STACK_SIZE: usize = 64 * 1024;

static RUNTIME: AtomicPtr<RuntimeController> = AtomicPtr::new(ptr::null_mut());
static RUNTIME_LOCK: Mutex<()> = Mutex::new(());

#[derive(Clone)]
struct RuntimeConfig {
    api: ClawApiConfig,
    persistence: AgentPersistenceConfig,
}

struct RuntimeController {
    config: RuntimeConfig,
    responses: Arc<ResponseStore>,
    next_request_id: AtomicU32,
    sender: Option<Sender<RuntimeCommand>>,
    worker: Option<WorkerHandle>,
}

#[derive(Default)]
struct ResponseStore {
    responses: Mutex<HashMap<u32, SubmitResponse>>,
    ready: Condvar,
}

impl ResponseStore {
    fn finish(&self, response: SubmitResponse) {
        let mut responses = self
            .responses
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        responses.insert(response.request_id, response);
        self.ready.notify_all();
    }

    fn receive(
        &self,
        session_id: u32,
        request_id: u32,
        timeout_ms: u32,
    ) -> Result<SubmitResponse, CabiError> {
        let mut responses = self
            .responses
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
        if let Some(response) = responses.get(&request_id) {
            if response.session_id != session_id {
                return Err(CabiError::InvalidArgument);
            }
        }
        if let Some(response) = responses.remove(&request_id) {
            return Ok(response);
        }
        if timeout_ms == 0 {
            return Err(CabiError::Timeout);
        }

        let timeout = Duration::from_millis(u64::from(timeout_ms));
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or(CabiError::InvalidArgument)?;
        loop {
            let now = Instant::now();
            if now >= deadline {
                return Err(CabiError::Timeout);
            }
            let remaining = deadline.duration_since(now);
            let (guard, timed_out) = wait_timeout(&self.ready, responses, remaining);
            responses = guard;
            if let Some(response) = responses.get(&request_id) {
                if response.session_id != session_id {
                    return Err(CabiError::InvalidArgument);
                }
            }
            if let Some(response) = responses.remove(&request_id) {
                return Ok(response);
            }
            if timed_out {
                return Err(CabiError::Timeout);
            }
        }
    }
}

fn wait_timeout<'a>(
    ready: &Condvar,
    guard: MutexGuard<'a, HashMap<u32, SubmitResponse>>,
    timeout: Duration,
) -> (MutexGuard<'a, HashMap<u32, SubmitResponse>>, bool) {
    match ready.wait_timeout(guard, timeout) {
        Ok((guard, status)) => (guard, status.timed_out()),
        Err(poison) => {
            let (guard, status) = poison.into_inner();
            (guard, status.timed_out())
        }
    }
}

#[derive(Clone, Debug)]
struct SubmitInput {
    text: String,
}

struct SubmitResponse {
    request_id: u32,
    session_id: u32,
    status: ResponseStatus,
    text: String,
    error_message: String,
}

enum ResponseStatus {
    Ok,
    Error,
}

struct CompletedSubmit {
    store_response: bool,
    response: SubmitResponse,
}

struct RuntimeState {
    agent: DeviceAgent,
    responses: Arc<ResponseStore>,
}

enum RuntimeCommand {
    Submit {
        request_id: u32,
        session: SessionId,
        input: SubmitInput,
        store_response: bool,
        reply: mpsc::Sender<Result<(), CabiError>>,
    },
    CreateSession {
        reply: mpsc::Sender<Result<SessionId, CabiError>>,
    },
    ListSessions {
        reply: mpsc::Sender<Result<Vec<SessionId>, CabiError>>,
    },
    DeleteSession {
        session: SessionId,
        reply: mpsc::Sender<Result<(), CabiError>>,
    },
    Stop {
        reply: mpsc::Sender<Result<(), CabiError>>,
    },
}

enum WorkerEvent {
    Command(Option<RuntimeCommand>),
    InflightDone(CompletedSubmit),
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
/// Receive one completed response.
///
/// # Safety
/// `out_response` must point to writable memory for one response.
pub unsafe extern "C" fn claw_agent_session_receive(
    session_id: u32,
    request_id: u32,
    out_response: *mut ClawAgentResponse,
    timeout_ms: u32,
) -> EspErr {
    ffi_result(|| receive(session_id, request_id, out_response, timeout_ms))
}

#[no_mangle]
/// Release strings owned by a response returned from `claw_agent_session_receive`.
///
/// # Safety
/// `response` must be null or a response returned by `claw_agent_session_receive`.
pub unsafe extern "C" fn claw_agent_session_response_free(response: *mut ClawAgentResponse) {
    free_response(response);
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
        responses: Arc::new(ResponseStore::default()),
        next_request_id: AtomicU32::new(1),
        sender: None,
        worker: None,
    });
    RUNTIME.store(Box::into_raw(runtime), Ordering::Release);
    Ok(())
}

fn start() -> Result<(), CabiError> {
    let ready_receiver = {
        let _guard = lock_runtime();
        let runtime = runtime_mut()?;
        if runtime.sender.is_some() {
            return Ok(());
        }

        let (sender, receiver) = async_channel::unbounded();
        let (ready_sender, ready_receiver) = mpsc::channel();
        let config = runtime.config.clone();
        let responses = Arc::clone(&runtime.responses);
        let worker = EspIdfThread.spawn_worker(
            "claw_agent",
            AGENT_WORKER_STACK_SIZE,
            Priority::Normal,
            CoreAffinity::Any,
            move || run_worker(config, responses, receiver, ready_sender),
        )?;
        runtime.sender = Some(sender);
        runtime.worker = Some(worker);
        ready_receiver
    };

    let result = ready_receiver.recv().map_err(|_| CabiError::InvalidState)?;
    if result.is_err() {
        let worker = {
            let _guard = lock_runtime();
            let runtime = runtime_mut()?;
            runtime.sender = None;
            runtime.worker.take()
        };
        if let Some(worker) = worker {
            worker.join();
        }
    }
    result
}

fn stop() -> Result<(), CabiError> {
    let Some((sender, worker)) = take_running()? else {
        return Ok(());
    };
    stop_worker(sender, worker)
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
    let result = match (runtime.sender.take(), runtime.worker.take()) {
        (Some(sender), Some(worker)) => stop_worker(sender, worker),
        (_, Some(worker)) => {
            worker.join();
            Ok(())
        }
        _ => Ok(()),
    };
    drop(runtime);
    result
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
        SubmitInput {
            text: required_string(text)?,
        },
        !out_request_id.is_null(),
    )?;
    if let Some(out_request_id) = unsafe { out_request_id.as_mut() } {
        *out_request_id = request_id;
    }
    Ok(())
}

fn session_create(out_session_id: *mut u32) -> Result<(), CabiError> {
    let out_session_id = unsafe { out_session_id.as_mut() }.ok_or(CabiError::InvalidArgument)?;
    let sender = runtime_sender()?;
    let (reply, receiver) = mpsc::channel();
    sender
        .try_send(RuntimeCommand::CreateSession { reply })
        .map_err(|_| CabiError::InvalidState)?;
    let session = receiver.recv().map_err(|_| CabiError::InvalidState)??;
    *out_session_id = session.0;
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

    let sender = runtime_sender()?;
    let (reply, receiver) = mpsc::channel();
    sender
        .try_send(RuntimeCommand::ListSessions { reply })
        .map_err(|_| CabiError::InvalidState)?;
    let sessions = receiver.recv().map_err(|_| CabiError::InvalidState)??;
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

    let sender = runtime_sender()?;
    let (reply, receiver) = mpsc::channel();
    sender
        .try_send(RuntimeCommand::DeleteSession {
            session: SessionId::new(session_id),
            reply,
        })
        .map_err(|_| CabiError::InvalidState)?;
    receiver.recv().map_err(|_| CabiError::InvalidState)?
}

fn submit_owned(
    session: SessionId,
    input: SubmitInput,
    store_response: bool,
) -> Result<u32, CabiError> {
    let (sender, request_id) = runtime_submit_sender()?;
    let (reply, receiver) = mpsc::channel();
    sender
        .try_send(RuntimeCommand::Submit {
            request_id,
            session,
            input,
            store_response,
            reply,
        })
        .map_err(|_| CabiError::InvalidState)?;
    receiver.recv().map_err(|_| CabiError::InvalidState)??;
    Ok(request_id)
}

fn receive(
    session_id: u32,
    request_id: u32,
    out_response: *mut ClawAgentResponse,
    timeout_ms: u32,
) -> Result<(), CabiError> {
    if session_id == 0 || request_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let out_response = unsafe { out_response.as_mut() }.ok_or(CabiError::InvalidArgument)?;
    let response = runtime_responses()?.receive(session_id, request_id, timeout_ms)?;
    write_response(out_response, response)
}

fn run_worker(
    config: RuntimeConfig,
    responses: Arc<ResponseStore>,
    receiver: Receiver<RuntimeCommand>,
    ready: mpsc::Sender<Result<(), CabiError>>,
) {
    executor::run(worker_loop(config, responses, receiver, ready));
}

async fn worker_loop(
    config: RuntimeConfig,
    responses: Arc<ResponseStore>,
    receiver: Receiver<RuntimeCommand>,
    ready: mpsc::Sender<Result<(), CabiError>>,
) {
    // `async_channel::Receiver` is `!Unpin` (it holds a pinned event listener), so
    // it must be pinned once before it can be polled as a `Stream` below.
    let mut receiver = core::pin::pin!(receiver);
    let mut state = match RuntimeState::new(config, responses) {
        Ok(state) => state,
        Err(error) => {
            let _ = ready.send(Err(error));
            return;
        }
    };
    if let Err(error) = state.start() {
        let _ = ready.send(Err(error));
        return;
    }
    let _ = ready.send(Ok(()));

    let mut inflight = VecDeque::new();
    let mut receiver_open = true;
    let mut stop_reply: Option<mpsc::Sender<Result<(), CabiError>>> = None;

    loop {
        if stop_reply.is_some() && inflight.is_empty() {
            let result = state.stop();
            if let Some(reply) = stop_reply.take() {
                let _ = reply.send(result);
            }
            return;
        }
        if !receiver_open && inflight.is_empty() {
            let _ = state.stop();
            return;
        }

        let recv = receiver_open.then(|| receiver.as_mut());
        match (WorkerPoll {
            inflight: &mut inflight,
            recv,
        })
        .await
        {
            WorkerEvent::InflightDone(completed) => {
                if completed.store_response {
                    state.finish_response(completed.response);
                }
            }
            WorkerEvent::Command(Some(RuntimeCommand::Submit {
                request_id,
                session,
                input,
                store_response,
                reply,
            })) => {
                if stop_reply.is_some() {
                    let _ = reply.send(Err(CabiError::InvalidState));
                } else if let Some(future) =
                    state.start_submit(request_id, session, input, store_response, reply)
                {
                    inflight.push_back(future);
                }
            }
            WorkerEvent::Command(Some(RuntimeCommand::CreateSession { reply })) => {
                if stop_reply.is_some() {
                    let _ = reply.send(Err(CabiError::InvalidState));
                } else {
                    let _ = reply.send(Ok(state.agent.new_session()));
                }
            }
            WorkerEvent::Command(Some(RuntimeCommand::ListSessions { reply })) => {
                if stop_reply.is_some() {
                    let _ = reply.send(Err(CabiError::InvalidState));
                } else {
                    let _ = reply.send(Ok(state.agent.list_sessions()));
                }
            }
            WorkerEvent::Command(Some(RuntimeCommand::DeleteSession { session, reply })) => {
                if stop_reply.is_some() {
                    let _ = reply.send(Err(CabiError::InvalidState));
                } else {
                    let _ = reply.send(
                        state
                            .agent
                            .delete_session(session)
                            .map_err(|_| CabiError::NotFound),
                    );
                }
            }
            WorkerEvent::Command(Some(RuntimeCommand::Stop { reply })) => {
                stop_reply = Some(reply);
            }
            WorkerEvent::Command(None) => {
                receiver_open = false;
            }
        }
    }
}

impl RuntimeState {
    fn new(config: RuntimeConfig, responses: Arc<ResponseStore>) -> Result<Self, CabiError> {
        let agent = Rc::new(AgentSystem::new(config.api, config.persistence)?);
        register_capability_tools(agent.tool_registry())?;
        Ok(Self { agent, responses })
    }

    fn start(&self) -> Result<(), CabiError> {
        self.agent.start_all()?;
        Ok(())
    }

    fn stop(&self) -> Result<(), CabiError> {
        self.agent.stop_all()?;
        Ok(())
    }

    fn finish_response(&self, response: SubmitResponse) {
        self.responses.finish(response);
    }

    fn start_submit(
        &mut self,
        request_id: u32,
        session: SessionId,
        input: SubmitInput,
        store_response: bool,
        reply: mpsc::Sender<Result<(), CabiError>>,
    ) -> Option<InflightSubmit> {
        match self.build_submit(request_id, session, input) {
            Ok(submit) => {
                let _ = reply.send(Ok(()));
                Some(Box::pin(
                    async move { run_submit(submit, store_response).await },
                ))
            }
            Err(error) => {
                let _ = reply.send(Err(error));
                None
            }
        }
    }

    fn build_submit(
        &mut self,
        request_id: u32,
        session: SessionId,
        input: SubmitInput,
    ) -> Result<SubmitTask, CabiError> {
        if !self.agent.list_sessions().contains(&session) {
            return Err(CabiError::NotFound);
        }
        let capability_context = CapabilityContextData {
            request_id,
            ..CapabilityContextData::default()
        };
        Ok(SubmitTask {
            request_id,
            agent: Rc::clone(&self.agent),
            session,
            text: input.text,
            capability_context,
        })
    }
}

struct SubmitTask {
    request_id: u32,
    agent: DeviceAgent,
    session: SessionId,
    text: String,
    capability_context: CapabilityContextData,
}

async fn run_submit(task: SubmitTask, store_response: bool) -> CompletedSubmit {
    let request_id = task.request_id;
    let session_id = task.session.0;
    let context = task.capability_context.clone();
    let result: Result<String, CabiError> = with_capability_context(context, async move {
        let output = task
            .agent
            .submit(task.session, task.text, DeliveryKind::Interrupt)
            .await?;
        let text = response_text(&output);
        Ok(text)
    })
    .await;
    CompletedSubmit {
        store_response,
        response: match result {
            Ok(text) => SubmitResponse {
                request_id,
                session_id,
                status: ResponseStatus::Ok,
                text,
                error_message: String::new(),
            },
            Err(error) => SubmitResponse {
                request_id,
                session_id,
                status: ResponseStatus::Error,
                text: String::new(),
                error_message: error.to_string(),
            },
        },
    }
}

struct WorkerPoll<'a> {
    inflight: &'a mut VecDeque<InflightSubmit>,
    recv: Option<Pin<&'a mut Receiver<RuntimeCommand>>>,
}

impl Future for WorkerPoll<'_> {
    type Output = WorkerEvent;

    fn poll(mut self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let count = self.inflight.len();
        for _ in 0..count {
            let Some(mut future) = self.inflight.pop_front() else {
                break;
            };
            if let Poll::Ready(completed) = future.as_mut().poll(context) {
                return Poll::Ready(WorkerEvent::InflightDone(completed));
            }
            self.inflight.push_back(future);
        }

        // Poll the command `Receiver` as a `Stream`: `Some(cmd)` for a queued
        // command, `None` once every sender has dropped (same "closed" signal the
        // old channel gave).
        match self.recv.as_mut() {
            Some(receiver) => receiver.as_mut().poll_next(context).map(WorkerEvent::Command),
            None => Poll::Pending,
        }
    }
}

fn stop_worker(sender: Sender<RuntimeCommand>, worker: WorkerHandle) -> Result<(), CabiError> {
    let (reply, receiver) = mpsc::channel();
    let result = match sender.try_send(RuntimeCommand::Stop { reply }) {
        Ok(()) => receiver.recv().map_err(|_| CabiError::InvalidState)?,
        Err(_) => Err(CabiError::InvalidState),
    };
    drop(sender);
    worker.join();
    result
}

fn take_running() -> Result<Option<(Sender<RuntimeCommand>, WorkerHandle)>, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let Some(sender) = runtime.sender.take() else {
        return Ok(None);
    };
    let Some(worker) = runtime.worker.take() else {
        return Ok(None);
    };
    Ok(Some((sender, worker)))
}

fn runtime_submit_sender() -> Result<(Sender<RuntimeCommand>, u32), CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let sender = runtime.sender.clone().ok_or(CabiError::InvalidState)?;
    let request_id = runtime.next_request_id.fetch_add(1, Ordering::AcqRel);
    if request_id == 0 {
        return Err(CabiError::InvalidState);
    }
    Ok((sender, request_id))
}

fn runtime_sender() -> Result<Sender<RuntimeCommand>, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    runtime.sender.clone().ok_or(CabiError::InvalidState)
}

fn runtime_responses() -> Result<Arc<ResponseStore>, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    Ok(Arc::clone(&runtime.responses))
}

fn runtime_mut() -> Result<&'static mut RuntimeController, CabiError> {
    let ptr = RUNTIME.load(Ordering::Acquire);
    if ptr.is_null() {
        return Err(CabiError::InvalidState);
    }
    Ok(unsafe { &mut *ptr })
}

fn response_text(output: &DriveOutput) -> String {
    let mut text = String::new();
    for reply in &output.replies {
        if reply.text.trim().is_empty() {
            continue;
        }
        if !text.is_empty() {
            text.push_str("\n\n");
        }
        text.push_str(&reply.text);
    }
    text
}

fn write_response(
    out_response: &mut ClawAgentResponse,
    response: SubmitResponse,
) -> Result<(), CabiError> {
    let text = cstring_raw(&response.text)?;
    let error_message = cstring_raw(&response.error_message)?;
    *out_response = ClawAgentResponse {
        status: match response.status {
            ResponseStatus::Ok => CLAW_AGENT_RESPONSE_STATUS_OK,
            ResponseStatus::Error => CLAW_AGENT_RESPONSE_STATUS_ERROR,
        },
        text,
        error_message,
    };
    Ok(())
}

fn free_response(response: *mut ClawAgentResponse) {
    let Some(response) = (unsafe { response.as_mut() }) else {
        return;
    };
    free_cstring(response.text);
    free_cstring(response.error_message);
    response.text = ptr::null_mut();
    response.error_message = ptr::null_mut();
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
    Thread(#[from] std::io::Error),
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
            Self::Thread(_) | Self::Agent(_) | Self::Tool(_) | Self::CapTool(_) => ESP_FAIL,
        }
    }
}
