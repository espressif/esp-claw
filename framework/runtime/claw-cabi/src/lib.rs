//! ESP-IDF C adapter for the Rust agent runtime.

#[cfg(target_os = "espidf")]
mod abi;
#[cfg(target_os = "espidf")]
mod executor;
#[cfg(target_os = "espidf")]
mod runtime;
#[cfg(target_os = "espidf")]
mod tool;

#[cfg(target_os = "espidf")]
pub use runtime::{
    claw_agent_cap_execute, claw_agent_deinit, claw_agent_init, claw_agent_receive,
    claw_agent_response_free, claw_agent_start, claw_agent_stop, claw_agent_submit,
};

#[cfg(not(target_os = "espidf"))]
mod host_stub {}
