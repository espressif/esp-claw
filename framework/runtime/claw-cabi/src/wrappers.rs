//! Turning the C descriptor (`abi`) into the internal capability types (all
//! re-exported from `claw_agent`): a `Tool` role becomes a [`ToolHandler`], a
//! `Channel` a [`ChannelAdapter`], and the lifecycle hooks a [`Lifecycle`]. Each
//! wrapper holds raw C callback pointers + the opaque `user_context`.

use core::ffi::{c_char, CStr};
use core::ptr;
use std::ffi::CString;
use std::sync::Arc;

use claw_agent::{
    Capability, CapabilityError, CapabilityGroup, ChannelAdapter, ChannelRuntime, InboundMessage,
    Lifecycle, OutboundMessage, Tool, ToolError, ToolHandler, ToolInvocation, ToolInvokeError,
    ToolOutput,
};

use crate::abi::{
    ClawCapability, ClawCapabilityChannelCloseCallback, ClawCapabilityChannelOpenCallback,
    ClawCapabilityExecuteCallback, ClawCapabilityGroup, ClawCapabilityLifecycle,
    ClawCapabilityLifecycleCallback, ClawCapabilityRole, ClawCapabilitySendCallback,
    ClawInboundMessage, UserContext, CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY,
};
use crate::result::into_result;
use crate::ClawChannelRuntime;

// --- C-string helpers -------------------------------------------------------

/// Copy a non-null, UTF-8 C string into an owned `String`.
///
/// # Safety
/// `pointer` must be null or a valid NUL-terminated C string.
unsafe fn required_string(pointer: *const c_char) -> Result<String, CapabilityError> {
    if pointer.is_null() {
        return Err(CapabilityError::InvalidArg);
    }
    CStr::from_ptr(pointer)
        .to_str()
        .map(str::to_string)
        .map_err(|_| CapabilityError::InvalidArg)
}

/// Like [`required_string`] but also rejects the empty string (for ids).
///
/// # Safety
/// See [`required_string`].
unsafe fn required_id(pointer: *const c_char) -> Result<String, CapabilityError> {
    let value = required_string(pointer)?;
    if value.is_empty() {
        return Err(CapabilityError::InvalidArg);
    }
    Ok(value)
}

/// A nullable C string: null => `None`.
///
/// # Safety
/// See [`required_string`].
unsafe fn optional_string(pointer: *const c_char) -> Result<Option<String>, CapabilityError> {
    if pointer.is_null() {
        return Ok(None);
    }
    Ok(Some(required_string(pointer)?))
}

/// Copy a borrowed C message (null => empty) returned by a callback.
///
/// # Safety
/// `pointer` must be null or a valid C string valid for this call.
unsafe fn copy_message(pointer: *const c_char) -> String {
    if pointer.is_null() {
        return String::new();
    }
    CStr::from_ptr(pointer).to_string_lossy().into_owned()
}

// --- Tool role --------------------------------------------------------------

/// A `claw_tool` tool backed by a C `execute` callback.
struct CTool {
    /// Tool name == capability id.
    name: String,
    /// OpenAI function schema JSON text.
    schema: String,
    execute: ClawCapabilityExecuteCallback,
    user_context: UserContext,
}

impl ToolHandler for CTool {
    fn name(&self) -> &str {
        &self.name
    }

    fn schema(&self) -> &str {
        &self.schema
    }

    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        let arguments = CString::new(call.arguments_json).map_err(|_| {
            ToolInvokeError::new(ToolError::InvalidArgumentsJson(
                "arguments contained an interior NUL byte".to_string(),
            ))
        })?;

        let mut buffer = vec![0u8; CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY];
        let mut output_length: usize = 0;
        let mut output_success: bool = false;

        // SAFETY: pointers are valid for the call; `buffer` is `output_capacity`
        // writable bytes; the out-params are live locals.
        let result = unsafe {
            (self.execute)(
                arguments.as_ptr(),
                buffer.as_mut_ptr().cast::<c_char>(),
                buffer.len(),
                &mut output_length,
                &mut output_success,
                self.user_context.0,
            )
        };

        if result.kind != crate::result::ClawCapabilityErrorKind::Ok {
            // SAFETY: borrowed message valid for this call.
            let message = unsafe { copy_message(result.message) };
            return Err(ToolError::invoke_rejected(message).into());
        }

        let length = output_length.min(buffer.len());
        let bytes = buffer.get(..length).unwrap_or(&[]);
        Ok(ToolOutput {
            output: String::from_utf8_lossy(bytes).into_owned(),
            ok: output_success,
        })
    }
}

// --- Channel role -----------------------------------------------------------

/// A C-backed bidirectional channel adapter.
struct CChannel {
    channel_id: String,
    open: ClawCapabilityChannelOpenCallback,
    close: ClawCapabilityChannelCloseCallback,
    send: ClawCapabilitySendCallback,
    user_context: UserContext,
}

impl ChannelAdapter for CChannel {
    fn channel_id(&self) -> &str {
        &self.channel_id
    }

    fn open(&self, runtime: Arc<dyn ChannelRuntime>) -> Result<(), CapabilityError> {
        let runtime = ClawChannelRuntime::into_raw(runtime);
        // SAFETY: callback is a valid fn pointer. Ownership of `runtime` is
        // transferred to C on success; on failure Rust reclaims it below.
        let result = unsafe { (self.open)(runtime, self.user_context.0) };
        // SAFETY: borrowed message valid for the call.
        match unsafe { into_result(result) } {
            Ok(()) => Ok(()),
            Err(error) => {
                unsafe { ClawChannelRuntime::drop_raw(runtime) };
                Err(error)
            }
        }
    }

    fn close(&self) -> Result<(), CapabilityError> {
        // SAFETY: callback is a valid fn pointer; message valid for the call.
        unsafe { into_result((self.close)(self.user_context.0)) }
    }

    fn send(&self, message: &OutboundMessage) -> Result<(), CapabilityError> {
        let channel = to_cstring(&message.channel)?;
        let chat_id = to_cstring(&message.chat_id)?;
        let text = to_cstring(&message.text)?;
        let reply_to = match &message.reply_to_message_id {
            Some(value) => Some(to_cstring(value)?),
            None => None,
        };
        let reply_to_pointer = reply_to
            .as_ref()
            .map(|value| value.as_ptr())
            .unwrap_or(ptr::null());

        // SAFETY: all pointers stay valid until the call returns.
        let result = unsafe {
            (self.send)(
                channel.as_ptr(),
                chat_id.as_ptr(),
                text.as_ptr(),
                reply_to_pointer,
                self.user_context.0,
            )
        };
        // SAFETY: borrowed message valid for the call.
        unsafe { into_result(result) }
    }
}

/// Build a `CString` from outbound text, mapping an interior NUL to a failure.
fn to_cstring(value: &str) -> Result<CString, CapabilityError> {
    CString::new(value).map_err(|_| {
        CapabilityError::Failed("outbound message contained an interior NUL byte".to_string())
    })
}

// --- Lifecycle --------------------------------------------------------------

/// A capability/group lifecycle backed by C hook callbacks.
struct CLifecycle {
    init: Option<ClawCapabilityLifecycleCallback>,
    start: Option<ClawCapabilityLifecycleCallback>,
    stop: Option<ClawCapabilityLifecycleCallback>,
    deinit: Option<ClawCapabilityLifecycleCallback>,
    user_context: UserContext,
}

impl CLifecycle {
    fn run(&self, hook: Option<ClawCapabilityLifecycleCallback>) -> Result<(), CapabilityError> {
        match hook {
            // SAFETY: the hook is a valid fn pointer; message valid for the call.
            Some(callback) => unsafe { into_result(callback(self.user_context.0)) },
            None => Ok(()),
        }
    }
}

impl Lifecycle for CLifecycle {
    fn init(&self) -> Result<(), CapabilityError> {
        self.run(self.init)
    }
    fn start(&self) -> Result<(), CapabilityError> {
        self.run(self.start)
    }
    fn stop(&self) -> Result<(), CapabilityError> {
        self.run(self.stop)
    }
    fn deinit(&self) -> Result<(), CapabilityError> {
        self.run(self.deinit)
    }
}

/// `Some(lifecycle)` when at least one hook is set, else `None`.
fn build_lifecycle(
    lifecycle: &ClawCapabilityLifecycle,
    user_context: UserContext,
) -> Option<Arc<dyn Lifecycle>> {
    if lifecycle.init.is_none()
        && lifecycle.start.is_none()
        && lifecycle.stop.is_none()
        && lifecycle.deinit.is_none()
    {
        return None;
    }
    Some(Arc::new(CLifecycle {
        init: lifecycle.init,
        start: lifecycle.start,
        stop: lifecycle.stop,
        deinit: lifecycle.deinit,
        user_context,
    }))
}

// --- Descriptor -> Capability ----------------------------------------------

/// Validate and convert one C descriptor into a [`Capability`].
///
/// # Safety
/// `descriptor` must be a valid `ClawCapability`: its string pointers null or
/// valid C strings, and the union arm named by `role` initialized.
pub(crate) unsafe fn build_capability(
    descriptor: &ClawCapability,
) -> Result<Capability, CapabilityError> {
    let id = required_id(descriptor.id)?;
    let description = optional_string(descriptor.description)?;
    let user_context = UserContext(descriptor.user_context);
    let lifecycle = build_lifecycle(&descriptor.lifecycle, user_context);

    // ROLE_NONE with no lifecycle would do nothing — reject it.
    if matches!(descriptor.role, ClawCapabilityRole::None) && lifecycle.is_none() {
        return Err(CapabilityError::InvalidArg);
    }

    let mut capability = match descriptor.role {
        ClawCapabilityRole::None => Capability::none(id),
        ClawCapabilityRole::Tool => {
            // SAFETY: role == Tool means the `tool` arm is the live one.
            let tool = unsafe { descriptor.role_data.tool };
            let execute = tool.execute.ok_or(CapabilityError::InvalidArg)?;
            let schema = required_string(tool.schema_json)?;
            Capability::tool(Tool::new(CTool {
                name: id.clone(),
                schema,
                execute,
                user_context,
            }))
        }
        ClawCapabilityRole::Channel => {
            // SAFETY: role == Channel means the `channel` arm is the live one.
            let channel = unsafe { descriptor.role_data.channel };
            let open = channel.open.ok_or(CapabilityError::InvalidArg)?;
            let close = channel.close.ok_or(CapabilityError::InvalidArg)?;
            let send = channel.send.ok_or(CapabilityError::InvalidArg)?;
            Capability::channel(Arc::new(CChannel {
                channel_id: id.clone(),
                open,
                close,
                send,
                user_context,
            }))
        }
    };

    if let Some(description) = description {
        capability = capability.with_description(description);
    }
    if let Some(lifecycle) = lifecycle {
        capability = capability.with_lifecycle(lifecycle);
    }
    Ok(capability)
}

/// Validate and convert a C group descriptor into a [`CapabilityGroup`].
///
/// # Safety
/// `group` must be a valid `ClawCapabilityGroup`: `members` either null (with
/// `member_count == 0`) or pointing at `member_count` valid descriptors.
pub(crate) unsafe fn build_group(
    group: &ClawCapabilityGroup,
) -> Result<CapabilityGroup, CapabilityError> {
    let id = required_id(group.id)?;

    let mut members = Vec::with_capacity(group.member_count);
    if !group.members.is_null() && group.member_count != 0 {
        // SAFETY: caller guarantees `member_count` valid descriptors at `members`.
        let descriptors = unsafe { core::slice::from_raw_parts(group.members, group.member_count) };
        for descriptor in descriptors {
            members.push(build_capability(descriptor)?);
        }
    }

    let lifecycle = build_lifecycle(&group.lifecycle, UserContext(group.user_context));
    let mut group = CapabilityGroup::new(id, members);
    if let Some(lifecycle) = lifecycle {
        group = group.with_lifecycle(lifecycle);
    }
    Ok(group)
}

/// Validate and convert a C inbound message into an [`InboundMessage`].
///
/// # Safety
/// `message` must be a valid `ClawInboundMessage`: each string pointer null
/// (only `sender_id`) or a valid C string.
pub(crate) unsafe fn build_inbound(
    message: &ClawInboundMessage,
) -> Result<InboundMessage, CapabilityError> {
    Ok(InboundMessage {
        message_id: required_id(message.message_id)?,
        channel: required_id(message.channel)?,
        chat_id: required_id(message.chat_id)?,
        sender_id: optional_string(message.sender_id)?,
        text: required_string(message.text)?,
    })
}
