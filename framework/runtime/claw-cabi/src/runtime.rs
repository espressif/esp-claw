use core::ffi::{c_char, CStr};
use core::future::Future;
use core::pin::Pin;
use core::ptr;
use core::task::{Context, Poll};
use std::collections::{HashMap, VecDeque};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::Rc;
use std::str::FromStr;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::{mpsc, Mutex, MutexGuard};

use claw_agent::{AgentError, AgentPersistenceConfig, AgentSystem};
use claw_api::{BackendKind, ClawApiConfig};
use claw_core::{DeliveryKind, DriveOutput, SessionId};
use claw_interface::{ClawThread, CoreAffinity, Priority, WorkerHandle};
use claw_sys::{EspIdfFs, EspIdfHttp, EspIdfThread, EspIdfTimer};
use claw_utils::{async_channel, AsyncReceiver, AsyncRecv, AsyncSender};
use serde_json::json;

use crate::abi::{
    ClawAgentConfig, ClawAgentInput, EspErr, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_FAIL,
    ESP_OK,
};
use crate::executor;
use crate::tool::{
    call_capability, register_capability_tools, with_capability_context, CapabilityContextData,
};

type DeviceAgent = Rc<AgentSystem<EspIdfFs, EspIdfHttp, EspIdfTimer>>;
type InflightSubmit = Pin<Box<dyn Future<Output = ()>>>;

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
    sender: Option<AsyncSender<RuntimeCommand>>,
    worker: Option<WorkerHandle>,
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

struct RuntimeState {
    agent: DeviceAgent,
    routes: HashMap<RouteKey, SessionId>,
    default_session: Option<SessionId>,
}

enum RuntimeCommand {
    Submit {
        input: SubmitInput,
        reply: mpsc::Sender<Result<(), CabiError>>,
    },
    Stop {
        reply: mpsc::Sender<Result<(), CabiError>>,
    },
}

enum WorkerEvent {
    Command(Option<RuntimeCommand>),
    InflightDone,
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
pub unsafe extern "C" fn claw_agent_submit(input: *const ClawAgentInput) -> EspErr {
    ffi_result(|| submit(input))
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
        let worker = EspIdfThread.spawn_worker(
            "claw_agent",
            AGENT_WORKER_STACK_SIZE,
            Priority::Normal,
            CoreAffinity::Any,
            move || run_worker(config, receiver, ready_sender),
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

fn submit(input: *const ClawAgentInput) -> Result<(), CabiError> {
    let input = unsafe { input.as_ref() }.ok_or(CabiError::InvalidArgument)?;
    submit_owned(SubmitInput {
        text: required_string(input.text)?,
        source_cap: optional_string(input.source_cap)?,
        source_channel: optional_string(input.source_channel)?,
        source_chat_id: optional_string(input.source_chat_id)?,
        target_channel: optional_string(input.target_channel)?,
        target_chat_id: optional_string(input.target_chat_id)?,
    })
}

fn submit_owned(input: SubmitInput) -> Result<(), CabiError> {
    let sender = runtime_sender()?;
    let (reply, receiver) = mpsc::channel();
    sender
        .send(RuntimeCommand::Submit { input, reply })
        .map_err(|_| CabiError::InvalidState)?;
    receiver.recv().map_err(|_| CabiError::InvalidState)?
}

fn run_worker(
    config: RuntimeConfig,
    receiver: AsyncReceiver<RuntimeCommand>,
    ready: mpsc::Sender<Result<(), CabiError>>,
) {
    executor::run(worker_loop(config, receiver, ready));
}

async fn worker_loop(
    config: RuntimeConfig,
    receiver: AsyncReceiver<RuntimeCommand>,
    ready: mpsc::Sender<Result<(), CabiError>>,
) {
    let mut state = match RuntimeState::new(config) {
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
            WorkerEvent::InflightDone => {}
            WorkerEvent::Command(Some(RuntimeCommand::Submit { input, reply })) => {
                if stop_reply.is_some() {
                    let _ = reply.send(Err(CabiError::InvalidState));
                } else if let Some(future) = state.start_submit(input, reply) {
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
    fn new(config: RuntimeConfig) -> Result<Self, CabiError> {
        let agent = Rc::new(AgentSystem::new(config.api, config.persistence)?);
        register_capability_tools(agent.tool_registry())?;
        Ok(Self {
            agent,
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

    fn start_submit(
        &mut self,
        input: SubmitInput,
        reply: mpsc::Sender<Result<(), CabiError>>,
    ) -> Option<InflightSubmit> {
        match self.build_submit(input) {
            Ok(submit) => Some(Box::pin(async move {
                let result = run_submit(submit).await;
                let _ = reply.send(result);
            })),
            Err(error) => {
                let _ = reply.send(Err(error));
                None
            }
        }
    }

    fn build_submit(&mut self, input: SubmitInput) -> Result<SubmitTask, CabiError> {
        let capability_context = capability_context(&input);
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
            agent: Rc::clone(&self.agent),
            session,
            text: input.text,
            reply_route,
            capability_context,
        })
    }
}

struct SubmitTask {
    agent: DeviceAgent,
    session: SessionId,
    text: String,
    reply_route: Option<RouteKey>,
    capability_context: CapabilityContextData,
}

async fn run_submit(task: SubmitTask) -> Result<(), CabiError> {
    let context = task.capability_context.clone();
    with_capability_context(context, async move {
        let output = task
            .agent
            .submit(task.session, task.text, DeliveryKind::Interrupt)
            .await?;
        route_output(task.reply_route.as_ref(), &output, &task.capability_context)?;
        Ok(())
    })
    .await
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
            if future.as_mut().poll(context).is_ready() {
                return Poll::Ready(WorkerEvent::InflightDone);
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

fn runtime_sender() -> Result<AsyncSender<RuntimeCommand>, CabiError> {
    let _guard = lock_runtime();
    let runtime = runtime_mut()?;
    runtime.sender.clone().ok_or(CabiError::InvalidState)
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

fn capability_context(input: &SubmitInput) -> CapabilityContextData {
    let route = route_key(input);
    let target = route_from(
        input.target_channel.as_deref(),
        input.target_chat_id.as_deref(),
    );
    CapabilityContextData {
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
            Self::Thread(_) | Self::Agent(_) | Self::Tool(_) | Self::CapTool(_) => ESP_FAIL,
        }
    }
}
