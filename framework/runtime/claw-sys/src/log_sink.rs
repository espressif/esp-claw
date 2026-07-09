//! Platform log sink — the device-side C↔Rust bridge that turns one `log::Level`
//! line into ESP-IDF's `ESP_LOGx`.
//!
//! `ESP_LOGx` are C macros, so a per-level C shim (`claw_rs_log_<level>`, see
//! `csrc/claw_rs_log.c`) bridges each level to the matching macro, yielding
//! ESP-IDF's standard timestamp/level/tag formatting and runtime level filtering.
//!
//! This is the device-only half of the sink: [`write`] exists only on the
//! `espidf` target. The host stderr fallback and the target dispatch between the
//! two live in the upper-layer `claw-log` crate, alongside the `log` facade
//! backend and the flat-tree `tracing` subscriber that drive this bridge.

#[cfg(target_os = "espidf")]
use core::ffi::c_char;
#[cfg(target_os = "espidf")]
use std::ffi::CString;

#[cfg(target_os = "espidf")]
use log::Level;

#[cfg(target_os = "espidf")]
extern "C" {
    /// Each is defined in `csrc/claw_rs_log.c` and forwards to the matching
    /// `ESP_LOGx(tag, "%s", msg)` macro.
    fn claw_rs_log_error(tag: *const c_char, msg: *const c_char);
    fn claw_rs_log_warn(tag: *const c_char, msg: *const c_char);
    fn claw_rs_log_info(tag: *const c_char, msg: *const c_char);
    fn claw_rs_log_debug(tag: *const c_char, msg: *const c_char);
    fn claw_rs_log_verbose(tag: *const c_char, msg: *const c_char);
}

/// Write one already-formatted line to ESP-IDF's `ESP_LOGx` at `level`, tagged
/// with `tag`. Device-only; `claw-log` calls this on `espidf` and falls back to
/// stderr on the host.
///
/// The caller passes a fully rendered message; this performs no level filtering
/// of its own (the `log` / `tracing` layers and ESP-IDF's runtime level do that).
#[cfg(target_os = "espidf")]
pub fn write(level: Level, tag: &str, msg: &str) {
    let c_string = |text: &str, replacement: u8| {
        let mut bytes = text.as_bytes().to_vec();
        for byte in &mut bytes {
            if *byte == 0 {
                *byte = replacement;
            }
        }
        // SAFETY: every interior NUL byte was replaced above.
        unsafe { CString::from_vec_unchecked(bytes) }
    };
    let tag_c = c_string(tag, b'_');
    let msg_c = c_string(msg, b' ');
    let (tag_ptr, msg_ptr) = (tag_c.as_ptr(), msg_c.as_ptr());
    // SAFETY: both pointers reference NUL-terminated C strings that stay alive
    // for the whole call; the shims only read them and return.
    unsafe {
        match level {
            Level::Error => claw_rs_log_error(tag_ptr, msg_ptr),
            Level::Warn => claw_rs_log_warn(tag_ptr, msg_ptr),
            Level::Info => claw_rs_log_info(tag_ptr, msg_ptr),
            Level::Debug => claw_rs_log_debug(tag_ptr, msg_ptr),
            Level::Trace => claw_rs_log_verbose(tag_ptr, msg_ptr),
        }
    }
}
