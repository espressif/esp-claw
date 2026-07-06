//! C ABI entry points for app-claw debug console wiring.

#![allow(unsafe_code)]

#[cfg(all(feature = "espidf", target_os = "espidf"))]
use debug_console::stream::EspIdf;
#[cfg(feature = "stdio")]
use debug_console::stream::Stdio;
#[cfg(any(feature = "stdio", all(feature = "espidf", target_os = "espidf")))]
use debug_console::ByteStream;

#[cfg(any(feature = "stdio", all(feature = "espidf", target_os = "espidf")))]
fn start_cabi<S>() -> i32
where
    S: ByteStream + Default,
{
    match crate::cli::start::<S>() {
        Ok(_) => crate::sys::ESP_OK,
        Err(_) => crate::sys::ESP_FAIL,
    }
}

/// Starts the app-claw debug console over the ESP-IDF console stream.
#[cfg(all(feature = "espidf", target_os = "espidf"))]
#[no_mangle]
pub extern "C" fn app_claw_debug_console_start_espidf() -> i32 {
    start_cabi::<EspIdf>()
}

/// Starts the app-claw debug console over process stdio.
#[cfg(feature = "stdio")]
#[no_mangle]
pub extern "C" fn app_claw_debug_console_start_stdio() -> i32 {
    start_cabi::<Stdio>()
}
