use core::ffi::{c_char, c_int};

pub type EspErr = c_int;

pub const ESP_OK: EspErr = 0;
pub const ESP_FAIL: EspErr = -1;
pub const ESP_ERR_INVALID_ARG: EspErr = 0x102;
pub const ESP_ERR_INVALID_STATE: EspErr = 0x103;
pub const ESP_ERR_INVALID_SIZE: EspErr = 0x104;
pub const ESP_ERR_NOT_FOUND: EspErr = 0x105;
pub const ESP_ERR_TIMEOUT: EspErr = 0x107;

pub const CLAW_CAP_KIND_CALLABLE: c_int = 0;
pub const CLAW_CAP_KIND_HYBRID: c_int = 2;

pub const CLAW_CAP_CALLER_AGENT: c_int = 1;

pub const CLAW_CAP_FLAG_CALLABLE_BY_LLM: u32 = 1 << 0;
pub const CLAW_CAP_FLAG_ROOT_AGENT_ONLY: u32 = 1 << 4;

pub const TOOL_OUTPUT_CAPACITY: usize = 16 * 1024;

#[repr(C)]
pub struct ClawAgentConfig {
    pub api_key: *const c_char,
    pub backend_type: *const c_char,
    pub model: *const c_char,
    pub base_url: *const c_char,
    pub persistence_dir: *const c_char,
    pub skills_root_dir: *const c_char,
    pub system_skills_root_dir: *const c_char,
}

/// Event kinds delivered by `claw_agent_session_receive`, one event per call.
///
/// `OUTPUT`/`REASONING`/`TOOLS` are content events: `text` carries an append
/// fragment (or one complete tool-call name). Their matching `_END` event
/// explicitly closes that content stream. `DONE` marks one root-visible turn
/// ending; the session stream stays open. `ERROR` carries `error_message`.
/// `INPUT_REQUESTED` pauses the current turn and carries `request_id`,
/// `input_kind`, and semantic data in `text`. `CLOSED` is terminal for the open
/// session stream.
pub const CLAW_AGENT_EVENT_KIND_OUTPUT: c_int = 0;
pub const CLAW_AGENT_EVENT_KIND_REASONING: c_int = 1;
pub const CLAW_AGENT_EVENT_KIND_TOOLS: c_int = 2;
pub const CLAW_AGENT_EVENT_KIND_DONE: c_int = 3;
pub const CLAW_AGENT_EVENT_KIND_ERROR: c_int = 4;
pub const CLAW_AGENT_EVENT_KIND_CLOSED: c_int = 5;
pub const CLAW_AGENT_EVENT_KIND_OUTPUT_END: c_int = 6;
pub const CLAW_AGENT_EVENT_KIND_REASONING_END: c_int = 7;
pub const CLAW_AGENT_EVENT_KIND_TOOLS_END: c_int = 8;
pub const CLAW_AGENT_EVENT_KIND_INPUT_REQUESTED: c_int = 9;

pub const CLAW_AGENT_INPUT_REQUEST_KIND_NONE: c_int = 0;
pub const CLAW_AGENT_INPUT_REQUEST_KIND_PERMISSION_APPROVAL: c_int = 1;

#[repr(C)]
pub struct ClawAgentEvent {
    pub kind: c_int,
    /// Session-local id for `INPUT_REQUESTED`; zero otherwise.
    pub request_id: u32,
    /// Semantic input kind for `INPUT_REQUESTED`; `NONE` otherwise.
    pub input_kind: c_int,
    /// Owned UTF-8 fragment for content events (`OUTPUT`/`REASONING`/`TOOLS`),
    /// or the semantic request summary for `INPUT_REQUESTED`; null otherwise.
    /// Released by `claw_agent_event_free`.
    pub text: *mut c_char,
    /// Owned UTF-8 message for `ERROR`; null otherwise. Released by
    /// `claw_agent_event_free`.
    pub error_message: *mut c_char,
}

#[repr(C)]
pub struct ClawCapCallContext {
    pub request_id: u32,
    pub session_id: *const c_char,
    pub agent_id: *const c_char,
    pub agent_type: *const c_char,
    pub parent_agent_id: *const c_char,
    pub parent_session_id: *const c_char,
    pub channel: *const c_char,
    pub chat_id: *const c_char,
    pub target_channel: *const c_char,
    pub target_chat_id: *const c_char,
    pub source_cap: *const c_char,
    pub correlation_id: *const c_char,
    pub caller: c_int,
}

impl Default for ClawCapCallContext {
    fn default() -> Self {
        Self {
            request_id: 0,
            session_id: core::ptr::null(),
            agent_id: core::ptr::null(),
            agent_type: core::ptr::null(),
            parent_agent_id: core::ptr::null(),
            parent_session_id: core::ptr::null(),
            channel: core::ptr::null(),
            chat_id: core::ptr::null(),
            target_channel: core::ptr::null(),
            target_chat_id: core::ptr::null(),
            source_cap: core::ptr::null(),
            correlation_id: core::ptr::null(),
            caller: CLAW_CAP_CALLER_AGENT,
        }
    }
}

pub type ClawCapLifecycleFn = Option<unsafe extern "C" fn() -> EspErr>;

pub type ClawCapExecuteFn = Option<
    unsafe extern "C" fn(
        input_json: *const c_char,
        ctx: *const ClawCapCallContext,
        output: *mut c_char,
        output_size: usize,
    ) -> EspErr,
>;

#[repr(C)]
pub struct ClawCapDescriptor {
    pub id: *const c_char,
    pub name: *const c_char,
    pub family: *const c_char,
    pub description: *const c_char,
    pub kind: c_int,
    pub cap_flags: u32,
    pub input_schema_json: *const c_char,
    pub init: ClawCapLifecycleFn,
    pub start: ClawCapLifecycleFn,
    pub stop: ClawCapLifecycleFn,
    pub execute: ClawCapExecuteFn,
}

#[repr(C)]
pub struct ClawCapList {
    pub items: *const ClawCapDescriptor,
    pub count: usize,
}

#[repr(C)]
pub struct ClawCapDescriptorInfo {
    pub id: *const c_char,
    pub name: *const c_char,
    pub group_id: *const c_char,
    pub state: c_int,
    pub active_calls: u32,
}

extern "C" {
    pub fn claw_cap_list() -> ClawCapList;
    pub fn claw_cap_get_descriptor_state(
        id_or_name: *const c_char,
        info: *mut ClawCapDescriptorInfo,
    ) -> EspErr;
    pub fn claw_cap_call(
        id_or_name: *const c_char,
        input_json: *const c_char,
        ctx: *const ClawCapCallContext,
        output: *mut c_char,
        output_size: usize,
    ) -> EspErr;
}
