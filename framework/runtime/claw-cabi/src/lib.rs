//! `claw-cabi` — the single outbound C ABI (Rust -> C) for the agent/capability
//! stack.
//!
//! To C, the only concept is a *capability*. The whole surface is three
//! functions in two planes:
//!
//! - **Control plane** — register capabilities:
//!   [`claw_capability_register`] / [`claw_capability_register_group`].
//! - **Data plane** — the receive half of a channel:
//!   [`claw_capability_ingress_push`] (the mirror of
//!   `claw_core::Orchestrator::push_user_message`; the outbound half is the
//!   capability descriptor's `send` callback).
//!
//! The two opaque handles ([`ClawCapabilityRegistry`], [`ClawCapabilityIngress`])
//! are created and destroyed on the Rust side and handed to C; lifecycle
//! *driving*, queries, and *building* the agent runtime are owned by Rust and
//! are not exposed to C.
//!
//! This is the one crate in the workspace where `unsafe` / `extern "C"` is
//! allowed; every other crate keeps `unsafe_code = "forbid"`. Every `extern "C"`
//! body runs under a panic guard so unwinding never crosses into C; strings are
//! borrowed both ways (see `result`).

mod abi;
mod result;
mod wrappers;

use std::sync::Arc;
#[cfg(target_os = "espidf")]
use std::sync::Mutex;

#[cfg(target_os = "espidf")]
use claw_agent::{AgentSystem, BackendKind, ClawApiConfig, PoolConfig, SessionId, SharedTaskPool};
use claw_agent::{CapabilityError, ChannelIngressSink, InboundMessage, Registry};
#[cfg(target_os = "espidf")]
use claw_sys::{EspIdfFs, EspIdfHttp, EspIdfThread};
#[cfg(target_os = "espidf")]
use core::{
    ffi::{c_char, CStr},
    ptr,
};

pub use abi::{
    ClawAgentSystemConfig, ClawCapability, ClawCapabilityChannel, ClawCapabilityExecuteCallback,
    ClawCapabilityGroup, ClawCapabilityLifecycle, ClawCapabilityLifecycleCallback,
    ClawCapabilityRole, ClawCapabilityRoleData, ClawCapabilitySendCallback, ClawCapabilityTool,
    ClawInboundMessage, CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY,
};
pub use result::{ClawCapabilityErrorKind, ClawCapabilityResult};

use crate::result::{from_result, guard};
use crate::wrappers::{build_capability, build_group, build_inbound};

/// Opaque agent runtime handle.
#[cfg(target_os = "espidf")]
pub struct ClawAgentSystem {
    system: AgentSystem,
    registry: Arc<Registry>,
    default_session: Mutex<Option<SessionId>>,
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

/// Opaque ingress handle: wraps `Arc<dyn claw_agent::ChannelIngressSink>`.
///
/// Created Rust-side after the runtime is wired (the sink is the `Orchestrator`)
/// and handed to C so channel gateways can push inbound messages.
pub struct ClawCapabilityIngress {
    sink: Arc<dyn ChannelIngressSink>,
}

impl ClawCapabilityIngress {
    /// Box a shared ingress sink into a raw handle for C.
    pub fn into_raw(sink: Arc<dyn ChannelIngressSink>) -> *mut ClawCapabilityIngress {
        Box::into_raw(Box::new(ClawCapabilityIngress { sink }))
    }

    /// Reclaim and drop a handle produced by [`into_raw`](Self::into_raw).
    ///
    /// # Safety
    /// `handle` must be null or a pointer returned by [`into_raw`](Self::into_raw)
    /// that has not already been dropped.
    pub unsafe fn drop_raw(handle: *mut ClawCapabilityIngress) {
        if !handle.is_null() {
            drop(Box::from_raw(handle));
        }
    }
}

/// Destroy an ingress handle returned by [`claw_agent_system_create`].
///
/// # Safety
/// `ingress` must be null or a live ingress handle not already destroyed.
#[no_mangle]
pub unsafe extern "C" fn claw_capability_ingress_destroy(
    ingress: *mut ClawCapabilityIngress,
) -> ClawCapabilityResult {
    guard(|| {
        unsafe { ClawCapabilityIngress::drop_raw(ingress) };
        crate::result::ok()
    })
}

#[cfg(target_os = "espidf")]
/// Build an ESP-IDF agent runtime from an already-populated registry.
///
/// This does not start capability lifecycle hooks. C can store the returned
/// ingress handle in channel contexts, then call [`claw_agent_system_start`].
///
/// # Safety
/// All pointers must be valid for the duration of the call. `registry` must be a
/// live registry handle. `ret_system` and `ret_ingress` must be valid
/// out-pointers.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_create(
    config: *const ClawAgentSystemConfig,
    registry: *mut ClawCapabilityRegistry,
    ret_system: *mut *mut ClawAgentSystem,
    ret_ingress: *mut *mut ClawCapabilityIngress,
) -> ClawCapabilityResult {
    guard(|| {
        from_result(unsafe { agent_system_create_inner(config, registry, ret_system, ret_ingress) })
    })
}

#[cfg(target_os = "espidf")]
unsafe fn agent_system_create_inner(
    config: *const ClawAgentSystemConfig,
    registry: *mut ClawCapabilityRegistry,
    ret_system: *mut *mut ClawAgentSystem,
    ret_ingress: *mut *mut ClawCapabilityIngress,
) -> Result<(), CapabilityError> {
    let config = config.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let registry_handle = registry.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let system_out = ret_system.as_mut().ok_or(CapabilityError::InvalidArg)?;
    let ingress_out = ret_ingress.as_mut().ok_or(CapabilityError::InvalidArg)?;
    *system_out = core::ptr::null_mut();
    *ingress_out = core::ptr::null_mut();

    let registry = Arc::clone(&registry_handle.registry);
    let llm = build_llm_config(config)?;
    let memory_dir = unsafe { required_string(config.memory_dir)? };
    let pool = Arc::new(
        SharedTaskPool::new(PoolConfig::default(), EspIdfThread)
            .map_err(|error| CapabilityError::Failed(error.to_string()))?,
    );

    let mut builder = AgentSystem::builder::<EspIdfFs, EspIdfHttp>()
        .llm(llm)
        .memory_dir(memory_dir)
        .task_pool(pool)
        .capabilities(Arc::clone(&registry));
    if let Some(channel) = unsafe { optional_string(config.default_channel)? } {
        builder = builder.channel(channel);
    }

    let system = builder
        .build()
        .map_err(|error| CapabilityError::Failed(error.to_string()))?;
    let ingress = system.ingress();

    *ingress_out = ClawCapabilityIngress::into_raw(ingress);
    *system_out = Box::into_raw(Box::new(ClawAgentSystem {
        system,
        registry,
        default_session: Mutex::new(None),
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
    handle.registry.start_all()
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
    handle.registry.stop_all()
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
    boxed.registry.stop_all()?;
    drop(boxed);
    Ok(())
}

#[cfg(target_os = "espidf")]
/// Send one text prompt synchronously through the Rust agent runtime.
///
/// `session_id` may be null, empty, or `"default"` to use an ABI-owned default
/// session. When `session_id_buffer` is provided, the actual session id is
/// copied back as `session-N`.
///
/// # Safety
/// `system` must be a live system handle. String pointers must be valid
/// NUL-terminated UTF-8 strings for the duration of this call. Output buffers
/// must be writable for their stated capacities.
#[no_mangle]
pub unsafe extern "C" fn claw_agent_system_send(
    system: *mut ClawAgentSystem,
    session_id: *const c_char,
    text: *const c_char,
    output_buffer: *mut c_char,
    output_capacity: usize,
    output_length: *mut usize,
    session_id_buffer: *mut c_char,
    session_id_capacity: usize,
    session_id_length: *mut usize,
) -> ClawCapabilityResult {
    guard(|| {
        from_result(unsafe {
            agent_system_send_inner(
                system,
                session_id,
                text,
                output_buffer,
                output_capacity,
                output_length,
                session_id_buffer,
                session_id_capacity,
                session_id_length,
            )
        })
    })
}

#[cfg(target_os = "espidf")]
#[allow(clippy::too_many_arguments)]
unsafe fn agent_system_send_inner(
    system: *mut ClawAgentSystem,
    session_id: *const c_char,
    text: *const c_char,
    output_buffer: *mut c_char,
    output_capacity: usize,
    output_length: *mut usize,
    session_id_buffer: *mut c_char,
    session_id_capacity: usize,
    session_id_length: *mut usize,
) -> Result<(), CapabilityError> {
    let handle = system.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let text = required_string(text)?;
    let session = resolve_session(handle, optional_string(session_id)?)?;
    let response = handle.system.send(session, text).join("\n");
    copy_string_to_c_buffer(&response, output_buffer, output_capacity, output_length)?;

    if !session_id_buffer.is_null() {
        copy_string_to_c_buffer(
            &session.to_wire(),
            session_id_buffer,
            session_id_capacity,
            session_id_length,
        )?;
    }

    Ok(())
}

#[cfg(target_os = "espidf")]
fn resolve_session(
    handle: &ClawAgentSystem,
    requested: Option<String>,
) -> Result<SessionId, CapabilityError> {
    if matches!(requested.as_deref(), None | Some("default")) {
        let mut default_session = handle
            .default_session
            .lock()
            .map_err(|_| CapabilityError::Failed("default session lock poisoned".to_string()))?;
        let session = match *default_session {
            Some(session) => session,
            None => {
                let session = handle.system.new_session();
                *default_session = Some(session);
                session
            }
        };
        return Ok(session);
    }

    let session_id = requested.ok_or(CapabilityError::InvalidArg)?;
    SessionId::from_wire(&session_id).map_err(|error| CapabilityError::Failed(error.to_string()))
}

#[cfg(target_os = "espidf")]
unsafe fn build_llm_config(
    config: &ClawAgentSystemConfig,
) -> Result<ClawApiConfig, CapabilityError> {
    let backend_type = required_string(config.backend_type)?
        .parse::<BackendKind>()
        .map_err(|_| CapabilityError::InvalidArg)?;

    let mut llm_config = ClawApiConfig::new(
        backend_type,
        required_string(config.api_key)?,
        required_string(config.model)?,
        required_string(config.base_url)?,
    );
    if config.timeout_ms != 0 {
        llm_config.timeout_ms = config.timeout_ms;
    }
    if config.max_tokens != 0 {
        llm_config.max_tokens = config.max_tokens;
    }
    if config.image_max_bytes != 0 {
        llm_config.image_max_bytes = config.image_max_bytes;
    }
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

/// Copy a nullable UTF-8 C string; null or empty becomes `None`.
///
/// # Safety
/// `pointer` must be null or a valid NUL-terminated C string.
#[cfg(target_os = "espidf")]
unsafe fn optional_string(pointer: *const c_char) -> Result<Option<String>, CapabilityError> {
    if pointer.is_null() {
        return Ok(None);
    }
    let value = CStr::from_ptr(pointer)
        .to_str()
        .map(str::to_string)
        .map_err(|_| CapabilityError::InvalidArg)?;
    if value.is_empty() {
        Ok(None)
    } else {
        Ok(Some(value))
    }
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

/// Deliver one inbound message to the orchestrator (the receive half of a
/// channel). The reply flows back out asynchronously through the channel's
/// `send` callback, not through this return value.
///
/// # Safety
/// `ingress` must be a handle from [`ClawCapabilityIngress::into_raw`] and
/// `message` a valid `ClawInboundMessage` for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn claw_capability_ingress_push(
    ingress: *mut ClawCapabilityIngress,
    message: *const ClawInboundMessage,
) -> ClawCapabilityResult {
    guard(|| from_result(unsafe { ingress_push_inner(ingress, message) }))
}

unsafe fn ingress_push_inner(
    ingress: *mut ClawCapabilityIngress,
    message: *const ClawInboundMessage,
) -> Result<(), CapabilityError> {
    let handle = ingress.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let descriptor = message.as_ref().ok_or(CapabilityError::InvalidArg)?;
    let inbound: InboundMessage = unsafe { build_inbound(descriptor)? };
    handle.sink.push_user_message(inbound);
    Ok(())
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

    use claw_agent::{InboundCommand, OutboundMessage, ToolInvocation};

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
                channel: ClawCapabilityChannel { send: None },
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
    struct RecordingSink {
        messages: Mutex<Vec<InboundMessage>>,
    }

    impl ChannelIngressSink for RecordingSink {
        fn push_user_message(&self, message: InboundMessage) {
            self.messages.lock().unwrap().push(message);
        }
        fn push_command(&self, _command: InboundCommand) {}
    }

    #[test]
    fn ingress_push_delivers_message() {
        let sink = Arc::new(RecordingSink::default());
        let handle =
            ClawCapabilityIngress::into_raw(Arc::clone(&sink) as Arc<dyn ChannelIngressSink>);

        let message = ClawInboundMessage {
            message_id: c"m1".as_ptr(),
            channel: c"local".as_ptr(),
            chat_id: c"chat".as_ptr(),
            sender_id: ptr::null(),
            session_id: c"session".as_ptr(),
            text: c"hello".as_ptr(),
        };
        let result = unsafe { claw_capability_ingress_push(handle, &message) };
        assert_eq!(result.kind, ClawCapabilityErrorKind::Ok);

        let received = sink.messages.lock().unwrap();
        assert_eq!(received.len(), 1);
        assert_eq!(received[0].text, "hello");
        assert_eq!(received[0].channel, "local");
        assert!(received[0].sender_id.is_none());

        drop(received);
        unsafe { ClawCapabilityIngress::drop_raw(handle) };
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

        let registry = Arc::new(Registry::new());
        let handle = ClawCapabilityRegistry::into_raw(Arc::clone(&registry));

        let descriptor = ClawCapability {
            id: c"local".as_ptr(),
            description: ptr::null(),
            role: ClawCapabilityRole::Channel,
            role_data: ClawCapabilityRoleData {
                channel: ClawCapabilityChannel {
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
