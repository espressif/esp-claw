//! ESP-IDF console byte stream.

#![allow(unsafe_code)]

use crate::{ByteStream, Error, Result};

/// Byte stream backed by the active ESP-IDF standard console.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct EspIdf;

impl ByteStream for EspIdf {
    fn read(&mut self, bytes: &mut [u8]) -> Result<usize> {
        ensure_initialized()?;
        if bytes.is_empty() {
            return Ok(0);
        }

        let mut read = 0_usize;
        let err =
            unsafe { claw_debug_console_espidf_read(bytes.as_mut_ptr(), bytes.len(), &mut read) };
        if err == ESP_OK {
            Ok(read)
        } else {
            Err(map_esp_error("read", err))
        }
    }

    fn write(&mut self, bytes: &[u8]) -> Result<usize> {
        ensure_initialized()?;
        if bytes.is_empty() {
            return Ok(0);
        }

        let mut written = 0_usize;
        let err =
            unsafe { claw_debug_console_espidf_write(bytes.as_ptr(), bytes.len(), &mut written) };
        if err == ESP_OK {
            Ok(written)
        } else {
            Err(map_esp_error("write", err))
        }
    }

    fn flush(&mut self) -> Result<()> {
        ensure_initialized()?;

        let err = unsafe { claw_debug_console_espidf_flush() };
        if err == ESP_OK {
            Ok(())
        } else {
            Err(map_esp_error("flush", err))
        }
    }
}

const ESP_OK: i32 = 0;

fn ensure_initialized() -> Result<()> {
    let err = unsafe { claw_debug_console_espidf_init() };
    if err == ESP_OK {
        Ok(())
    } else {
        Err(map_esp_error("initialize", err))
    }
}

fn map_esp_error(operation: &'static str, error: i32) -> Error {
    Error::Stream(format!("esp-idf console {operation} failed: {error}"))
}

extern "C" {
    fn claw_debug_console_espidf_init() -> i32;
    fn claw_debug_console_espidf_read(bytes: *mut u8, len: usize, out_read: *mut usize) -> i32;
    fn claw_debug_console_espidf_write(
        bytes: *const u8,
        len: usize,
        out_written: *mut usize,
    ) -> i32;
    fn claw_debug_console_espidf_flush() -> i32;
}
