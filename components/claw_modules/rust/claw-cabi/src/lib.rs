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
mod bridge;
mod result;
mod wrappers;

use std::sync::Arc;

use claw_capability::{CapabilityError, Registry};
use claw_core::{ChannelIngressSink, InboundMessage};

pub use abi::{
    ClawCapability, ClawCapabilityChannel, ClawCapabilityExecuteCallback, ClawCapabilityGroup,
    ClawCapabilityLifecycle, ClawCapabilityLifecycleCallback, ClawCapabilityRole,
    ClawCapabilityRoleData, ClawCapabilitySendCallback, ClawCapabilityTool, ClawInboundMessage,
    CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY,
};
pub use bridge::{register_channels, RegistryChannelTransport, RegistryResolver};
pub use result::{ClawCapabilityErrorKind, ClawCapabilityResult};

use crate::result::{from_result, guard};
use crate::wrappers::{build_capability, build_group, build_inbound};

/// Opaque registry handle: wraps `Arc<claw_capability::Registry>`.
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

/// Opaque ingress handle: wraps `Arc<dyn claw_core::ChannelIngressSink>`.
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

    use claw_capability::OutboundMessage;
    use claw_core::InboundCommand;
    use claw_tool::ToolInvocation;

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
