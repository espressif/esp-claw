//! The `#[repr(C)]` descriptor types and callback signatures, laid out to match
//! the hand-written `include/claw_cabi.h`. These are the wire structs C fills
//! in; `wrappers` turns them into the internal `claw_capability` types.

use core::ffi::{c_char, c_void};

use crate::result::ClawCapabilityResult;

/// Size of the buffer the registry hands a tool's `execute` callback to write
/// its output into. `output_capacity` always equals this value.
pub const CLAW_CAPABILITY_TOOL_OUTPUT_CAPACITY: usize = 4096;

/// An opaque C `user_context` pointer carried into every callback.
///
/// Raw pointers are neither `Send` nor `Sync`; the documented ABI contract is
/// that the C callbacks and their `user_context` are thread-safe (the registry
/// is driven from multiple tasks), so we assert it here.
#[derive(Clone, Copy)]
pub(crate) struct UserContext(pub(crate) *mut c_void);

// SAFETY: upheld by the ABI contract documented on the callbacks in
// `include/claw_cabi.h` ("C callbacks must be thread-safe").
unsafe impl Send for UserContext {}
unsafe impl Sync for UserContext {}

// The callback type aliases below name each signature for the Rust wrapper /
// test code (which store the *unwrapped* function pointer). The `#[repr(C)]`
// descriptor structs deliberately spell the `Option<extern "C" fn ...>` out
// inline instead of using these aliases: cbindgen renders `Option<FnAlias>` as
// an opaque struct, but resolves an inline `Option<extern "C" fn>` to a proper
// nullable C function pointer. The alias and the inline form are the same type.

/// `init`/`start`/`stop`/`deinit` hook: `claw_capability_lifecycle_callback_t`.
pub type ClawCapabilityLifecycleCallback =
    unsafe extern "C" fn(user_context: *mut c_void) -> ClawCapabilityResult;

/// Tool `execute`: `claw_capability_execute_callback_t`.
pub type ClawCapabilityExecuteCallback = unsafe extern "C" fn(
    arguments_json: *const c_char,
    output_buffer: *mut c_char,
    output_capacity: usize,
    output_length: *mut usize,
    output_success: *mut bool,
    user_context: *mut c_void,
) -> ClawCapabilityResult;

/// Channel outbound `send`: `claw_capability_send_callback_t`.
pub type ClawCapabilitySendCallback = unsafe extern "C" fn(
    channel: *const c_char,
    chat_id: *const c_char,
    text: *const c_char,
    reply_to_message_id: *const c_char,
    user_context: *mut c_void,
) -> ClawCapabilityResult;

/// `claw_capability_lifecycle_t`: four nullable hooks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClawCapabilityLifecycle {
    pub init: Option<unsafe extern "C" fn(user_context: *mut c_void) -> ClawCapabilityResult>,
    pub start: Option<unsafe extern "C" fn(user_context: *mut c_void) -> ClawCapabilityResult>,
    pub stop: Option<unsafe extern "C" fn(user_context: *mut c_void) -> ClawCapabilityResult>,
    pub deinit: Option<unsafe extern "C" fn(user_context: *mut c_void) -> ClawCapabilityResult>,
}

/// `claw_capability_role_t`: the live arm of [`ClawCapabilityRoleData`].
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClawCapabilityRole {
    None = 0,
    Tool = 1,
    Channel = 2,
}

/// `claw_capability_tool_t`: the `Tool` role payload.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClawCapabilityTool {
    pub schema_json: *const c_char,
    pub execute: Option<
        unsafe extern "C" fn(
            arguments_json: *const c_char,
            output_buffer: *mut c_char,
            output_capacity: usize,
            output_length: *mut usize,
            output_success: *mut bool,
            user_context: *mut c_void,
        ) -> ClawCapabilityResult,
    >,
}

/// `claw_capability_channel_t`: the `Channel` role payload.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ClawCapabilityChannel {
    pub send: Option<
        unsafe extern "C" fn(
            channel: *const c_char,
            chat_id: *const c_char,
            text: *const c_char,
            reply_to_message_id: *const c_char,
            user_context: *mut c_void,
        ) -> ClawCapabilityResult,
    >,
}

/// The role union; the live arm is named by [`ClawCapability::role`].
#[repr(C)]
#[derive(Clone, Copy)]
pub union ClawCapabilityRoleData {
    pub tool: ClawCapabilityTool,
    pub channel: ClawCapabilityChannel,
}

/// `claw_capability_t`: one capability descriptor (tagged union over the role).
#[repr(C)]
pub struct ClawCapability {
    pub id: *const c_char,
    pub description: *const c_char,
    pub role: ClawCapabilityRole,
    pub role_data: ClawCapabilityRoleData,
    pub lifecycle: ClawCapabilityLifecycle,
    pub user_context: *mut c_void,
}

/// `claw_capability_group_t`: a bundle sharing one optional group lifecycle.
#[repr(C)]
pub struct ClawCapabilityGroup {
    pub id: *const c_char,
    pub members: *const ClawCapability,
    pub member_count: usize,
    pub lifecycle: ClawCapabilityLifecycle,
    pub user_context: *mut c_void,
}

/// `claw_inbound_message_t`: maps field-for-field onto `claw_core::InboundMessage`.
#[repr(C)]
pub struct ClawInboundMessage {
    pub message_id: *const c_char,
    pub channel: *const c_char,
    pub chat_id: *const c_char,
    pub sender_id: *const c_char,
    pub session_id: *const c_char,
    pub text: *const c_char,
}

/// `claw_agent_system_config_t`: device runtime build inputs.
#[repr(C)]
pub struct ClawAgentSystemConfig {
    pub api_key: *const c_char,
    pub backend_type: *const c_char,
    pub model: *const c_char,
    pub base_url: *const c_char,
    pub auth_type: *const c_char,
    pub max_tokens_field: *const c_char,
    pub timeout_ms: u32,
    pub max_tokens: u32,
    pub image_max_bytes: usize,
    pub supports_tools: bool,
    pub supports_vision: bool,
    pub image_remote_url_only: bool,
    /// DATA-rooted directory for transcript files.
    pub transcript_dir: *const c_char,
    /// DATA-rooted directory for editable profile documents.
    pub profile_dir: *const c_char,
    /// DATA-rooted directory for global long-term memory.
    pub global_long_term_dir: *const c_char,
    /// DATA-rooted directory for the conversation agent's long-term memory.
    pub conversation_long_term_dir: *const c_char,
    /// DATA-rooted directory for the worker agent's long-term memory.
    pub worker_long_term_dir: *const c_char,
    /// Default egress channel id. Nullable => "claw".
    pub default_channel: *const c_char,
}
