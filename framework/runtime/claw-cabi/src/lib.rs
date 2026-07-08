//! ESP-IDF C adapter for the Rust agent runtime.

#[cfg(target_os = "espidf")]
mod abi;
#[cfg(target_os = "espidf")]
mod runtime;
#[cfg(target_os = "espidf")]
mod tool;

#[cfg(target_os = "espidf")]
pub use runtime::{
    claw_agent_deinit, claw_agent_event_free, claw_agent_init, claw_agent_session_cancel,
    claw_agent_session_close, claw_agent_session_create, claw_agent_session_delete,
    claw_agent_session_interrupt, claw_agent_session_list, claw_agent_session_open,
    claw_agent_session_receive, claw_agent_session_submit, claw_agent_start, claw_agent_stop,
};

#[cfg(not(target_os = "espidf"))]
mod host_stub {}
