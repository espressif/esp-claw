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
use claw_utils::{async_channel, AsyncReceiver, AsyncRecv, AsyncSender};
use serde_json::json;

use crate::abi::{
    ClawAgentConfig, ClawAgentInput, ClawAgentResponse, ClawCapCallContext, EspErr,
    CLAW_AGENT_RESPONSE_STATUS_ERROR, CLAW_AGENT_RESPONSE_STATUS_OK, ESP_ERR_INVALID_ARG,
    ESP_ERR_INVALID_STATE, ESP_ERR_TIMEOUT, ESP_FAIL, ESP_OK,
};
use crate::executor;
use crate::tool::{
    call_capability, register_capability_tools, with_capability_context, CapabilityContextData,
};

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
    sender: Option<AsyncSender<RuntimeCommand>>,
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

    fn receive(&self, request_id: u32, timeout_ms: u32) -> Result<SubmitResponse, CabiError> {
        let mut responses = self
            .responses
            .lock()
            .unwrap_or_else(|poison| poison.into_inner());
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

#[derive(Clone, Debug, Hash, PartialEq, Eq)]
struct RouteKey {
    channel: String,
    chat_id: String,
}

#[derive(Clone, Debug)]
struct SubmitInput {
    text: String,
    source_cap: Option<String>,
    source_channel: Option<String>,
    source_chat_id: Option<String>,
    target_channel: Option<String>,
    target_chat_id: Option<String>,
}

struct SubmitResponse {
    request_id: u32,
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
    routes: HashMap<RouteKey, SessionId>,
    default_session: Option<SessionId>,
}

enum RuntimeCommand {
    Submit {
        request_id: u32,
        input: SubmitInput,
        store_response: bool,
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
/// Submit one inbound message to the running agent.
///
/// # Safety
/// `input` must point to valid UTF-8 C strings for this call.
pub unsafe extern "C" fn claw_agent_submit(
    input: *const ClawAgentInput,
    out_request_id: *mut u32,
) -> EspErr {
    ffi_result(|| submit(input, out_request_id))
}

#[no_mangle]
/// Capability entry point that submits one inbound message to the agent.
///
/// Matches `claw_cap_execute_fn`: reads `text` from `input_json` and routing
/// fields from `ctx`, then schedules a fire-and-forget submit. On success the
/// assigned request id is written to `output` as `request_id=<n>`.
///
/// # Safety
/// `input_json` and `ctx` (including its string fields) must be valid for this
/// call, and `output` must point to `output_size` writable bytes.
pub unsafe extern "C" fn claw_agent_cap_execute(
    input_json: *const c_char,
    ctx: *const ClawCapCallContext,
    output: *mut c_char,
    output_size: usize,
) -> EspErr {
    ffi_result(|| cap_execute(input_json, ctx, output, output_size))
}

#[no_mangle]
/// Receive one completed response.
///
/// # Safety
/// `out_response` must point to writable memory for one response.
pub unsafe extern "C" fn claw_agent_receive(
    request_id: u32,
    out_response: *mut ClawAgentResponse,
    timeout_ms: u32,
) -> EspErr {
    ffi_result(|| receive(request_id, out_response, timeout_ms))
}

#[no_mangle]
/// Release strings owned by a response returned from `claw_agent_receive`.
///
/// # Safety
/// `response` must be null or a response returned by `claw_agent_receive`.
pub unsafe extern "C" fn claw_agent_response_free(response: *mut ClawAgentResponse) {
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
    let persistence = AgentPersistenceConfig::new(&persistence_dir);

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

        let (sender, receiver) = async_channel();
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

fn submit(input: *const ClawAgentInput, out_request_id: *mut u32) -> Result<(), CabiError> {
    let input = unsafe { input.as_ref() }.ok_or(CabiError::InvalidArgument)?;
    let request_id = submit_owned(
        SubmitInput {
            text: required_string(input.text)?,
            source_cap: optional_string(input.source_cap)?,
            source_channel: optional_string(input.source_channel)?,
            source_chat_id: optional_string(input.source_chat_id)?,
            target_channel: optional_string(input.target_channel)?,
            target_chat_id: optional_string(input.target_chat_id)?,
        },
        !out_request_id.is_null(),
    )?;
    if let Some(out_request_id) = unsafe { out_request_id.as_mut() } {
        *out_request_id = request_id;
    }
    Ok(())
}

fn cap_execute(
    input_json: *const c_char,
    ctx: *const ClawCapCallContext,
    output: *mut c_char,
    output_size: usize,
) -> Result<(), CabiError> {
    let ctx = unsafe { ctx.as_ref() };
    let input = SubmitInput {
        text: cap_input_text(input_json)?,
        source_cap: ctx.and_then(|ctx| optional_string(ctx.source_cap).ok().flatten()),
        source_channel: ctx.and_then(|ctx| optional_string(ctx.channel).ok().flatten()),
        source_chat_id: ctx.and_then(|ctx| optional_string(ctx.chat_id).ok().flatten()),
        target_channel: ctx.and_then(|ctx| optional_string(ctx.target_channel).ok().flatten()),
        target_chat_id: ctx.and_then(|ctx| optional_string(ctx.target_chat_id).ok().flatten()),
    };
    let request_id = submit_owned(input, false)?;
    write_cap_output(output, output_size, request_id);
    Ok(())
}

fn cap_input_text(input_json: *const c_char) -> Result<String, CabiError> {
    let Some(raw) = optional_string(input_json)? else {
        return Ok(String::new());
    };
    let trimmed = raw.trim();
    if trimmed.is_empty() {
        return Ok(String::new());
    }
    let value: serde_json::Value =
        serde_json::from_str(trimmed).map_err(|_| CabiError::InvalidArgument)?;
    Ok(value
        .get("text")
        .and_then(serde_json::Value::as_str)
        .unwrap_or_default()
        .to_owned())
}

fn write_cap_output(output: *mut c_char, output_size: usize, request_id: u32) {
    if output.is_null() || output_size == 0 {
        return;
    }
    let text = format!("request_id={request_id}");
    let bytes = text.as_bytes();
    let len = bytes.len().min(output_size - 1);
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), output.cast::<u8>(), len);
        *output.add(len) = 0;
    }
}

fn submit_owned(input: SubmitInput, store_response: bool) -> Result<u32, CabiError> {
    let (sender, request_id) = runtime_submit_sender()?;
    let (reply, receiver) = mpsc::channel();
    sender
        .send(RuntimeCommand::Submit {
            request_id,
            input,
            store_response,
            reply,
        })
        .map_err(|_| CabiError::InvalidState)?;
    receiver.recv().map_err(|_| CabiError::InvalidState)??;
    Ok(request_id)
}

fn receive(
    request_id: u32,
    out_response: *mut ClawAgentResponse,
    timeout_ms: u32,
) -> Result<(), CabiError> {
    if request_id == 0 {
        return Err(CabiError::InvalidArgument);
    }
    let out_response = unsafe { out_response.as_mut() }.ok_or(CabiError::InvalidArgument)?;
    let response = runtime_responses()?.receive(request_id, timeout_ms)?;
    write_response(out_response, response)
}

fn run_worker(
    config: RuntimeConfig,
    responses: Arc<ResponseStore>,
    receiver: AsyncReceiver<RuntimeCommand>,
    ready: mpsc::Sender<Result<(), CabiError>>,
) {
    executor::run(worker_loop(config, responses, receiver, ready));
}

async fn worker_loop(
    config: RuntimeConfig,
    responses: Arc<ResponseStore>,
    receiver: AsyncReceiver<RuntimeCommand>,
    ready: mpsc::Sender<Result<(), CabiError>>,
) {
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

        let recv = receiver_open.then(|| receiver.recv());
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
                input,
                store_response,
                reply,
            })) => {
                if stop_reply.is_some() {
                    let _ = reply.send(Err(CabiError::InvalidState));
                } else if let Some(future) =
                    state.start_submit(request_id, input, store_response, reply)
                {
                    inflight.push_back(future);
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
        Ok(Self {
            agent,
            responses,
            routes: HashMap::new(),
            default_session: None,
        })
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
        input: SubmitInput,
        store_response: bool,
        reply: mpsc::Sender<Result<(), CabiError>>,
    ) -> Option<InflightSubmit> {
        match self.build_submit(request_id, input) {
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
        input: SubmitInput,
    ) -> Result<SubmitTask, CabiError> {
        let capability_context = capability_context(request_id, &input);
        let route = route_key(&input);
        let session = match route.as_ref() {
            Some(route) => match self.routes.get(route).copied() {
                Some(session) => session,
                None => {
                    let session = self.agent.new_session();
                    self.routes.insert(route.clone(), session);
                    session
                }
            },
            None => match self.default_session {
                Some(session) => session,
                None => {
                    let session = self.agent.new_session();
                    self.default_session = Some(session);
                    session
                }
            },
        };
        let reply_route = reply_route(&input);
        Ok(SubmitTask {
            request_id,
            agent: Rc::clone(&self.agent),
            session,
            text: input.text,
            reply_route,
            capability_context,
        })
    }
}

struct SubmitTask {
    request_id: u32,
    agent: DeviceAgent,
    session: SessionId,
    text: String,
    reply_route: Option<RouteKey>,
    capability_context: CapabilityContextData,
}

async fn run_submit(task: SubmitTask, store_response: bool) -> CompletedSubmit {
    let request_id = task.request_id;
    let context = task.capability_context.clone();
    let result: Result<String, CabiError> = with_capability_context(context, async move {
        let output = task
            .agent
            .submit(task.session, task.text, DeliveryKind::Interrupt)
            .await?;
        let text = response_text(&output);
        route_output(task.reply_route.as_ref(), &output, &task.capability_context)?;
        Ok(text)
    })
    .await;
    CompletedSubmit {
        store_response,
        response: match result {
            Ok(text) => SubmitResponse {
                request_id,
                status: ResponseStatus::Ok,
                text,
                error_message: String::new(),
            },
            Err(error) => SubmitResponse {
                request_id,
                status: ResponseStatus::Error,
                text: String::new(),
                error_message: error.to_string(),
            },
        },
    }
}

struct WorkerPoll<'a, 'receiver> {
    inflight: &'a mut VecDeque<InflightSubmit>,
    recv: Option<AsyncRecv<'receiver, RuntimeCommand>>,
}

impl Future for WorkerPoll<'_, '_> {
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

        match self.recv.as_mut() {
            Some(recv) => Pin::new(recv).poll(context).map(WorkerEvent::Command),
            None => Poll::Pending,
        }
    }
}

fn stop_worker(sender: AsyncSender<RuntimeCommand>, worker: WorkerHandle) -> Result<(), CabiError> {
    let (reply, receiver) = mpsc::channel();
    let result = match sender.send(RuntimeCommand::Stop { reply }) {
        Ok(()) => receiver.recv().map_err(|_| CabiError::InvalidState)?,
        Err(_) => Err(CabiError::InvalidState),
    };
    drop(sender);
    worker.join();
    result
}

fn take_running() -> Result<Option<(AsyncSender<RuntimeCommand>, WorkerHandle)>, CabiError> {
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

fn runtime_submit_sender() -> Result<(AsyncSender<RuntimeCommand>, u32), CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    let sender = runtime.sender.clone().ok_or(CabiError::InvalidState)?;
    let request_id = runtime.next_request_id.fetch_add(1, Ordering::AcqRel);
    if request_id == 0 {
        return Err(CabiError::InvalidState);
    }
    Ok((sender, request_id))
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

fn route_output(
    route: Option<&RouteKey>,
    output: &DriveOutput,
    context: &CapabilityContextData,
) -> Result<(), CabiError> {
    let Some(route) = route else {
        return Ok(());
    };
    let Some(cap_name) = send_capability(&route.channel) else {
        return Ok(());
    };

    for reply in &output.replies {
        if reply.text.trim().is_empty() {
            continue;
        }
        let arguments = json!({
            "channel": route.channel,
            "chat_id": route.chat_id,
            "message": reply.text,
        })
        .to_string();
        call_capability(cap_name, &arguments, context)?;
    }
    Ok(())
}

fn capability_context(request_id: u32, input: &SubmitInput) -> CapabilityContextData {
    let route = route_key(input);
    let target = route_from(
        input.target_channel.as_deref(),
        input.target_chat_id.as_deref(),
    );
    CapabilityContextData {
        request_id,
        channel: route.as_ref().map(|route| route.channel.clone()),
        chat_id: route.as_ref().map(|route| route.chat_id.clone()),
        target_channel: target.as_ref().map(|route| route.channel.clone()),
        target_chat_id: target.as_ref().map(|route| route.chat_id.clone()),
        source_cap: input.source_cap.clone(),
    }
}

fn route_key(input: &SubmitInput) -> Option<RouteKey> {
    route_from(
        input.source_channel.as_deref(),
        input.source_chat_id.as_deref(),
    )
    .or_else(|| {
        route_from(
            input.target_channel.as_deref(),
            input.target_chat_id.as_deref(),
        )
    })
}

fn reply_route(input: &SubmitInput) -> Option<RouteKey> {
    route_from(
        input.target_channel.as_deref(),
        input.target_chat_id.as_deref(),
    )
    .or_else(|| {
        route_from(
            input.source_channel.as_deref(),
            input.source_chat_id.as_deref(),
        )
    })
}

fn route_from(channel: Option<&str>, chat_id: Option<&str>) -> Option<RouteKey> {
    let channel = channel?.trim();
    let chat_id = chat_id?.trim();
    if channel.is_empty() || chat_id.is_empty() {
        return None;
    }
    Some(RouteKey {
        channel: channel.to_owned(),
        chat_id: chat_id.to_owned(),
    })
}

fn send_capability(channel: &str) -> Option<&'static str> {
    match channel {
        "feishu" => Some("feishu_send_message"),
        "qq" => Some("qq_send_message"),
        "tg" | "telegram" => Some("tg_send_message"),
        "wechat" => Some("wechat_send_message"),
        "local" | "web" => Some("local_send_message"),
        _ => None,
    }
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
        request_id: response.request_id,
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
            Self::Timeout => ESP_ERR_TIMEOUT,
            Self::Thread(_) | Self::Agent(_) | Self::Tool(_) | Self::CapTool(_) => ESP_FAIL,
        }
    }
}
