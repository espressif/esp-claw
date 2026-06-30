//! The `Result`-shaped return: an error kind + a **borrowed** message, plus the
//! panic guard and the conversions between [`CapabilityError`] and the C value.
//!
//! Message ownership never crosses the ABI (see `DESIGN.md` §3):
//! - Structural kinds point at `'static` C strings.
//! - `Failed`'s dynamic text lives in a thread-local buffer, valid until the
//!   next claw-cabi call on the same thread. There is no `*_free`.

use core::ffi::{c_char, CStr};
use core::ptr;
use std::cell::RefCell;
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};

use claw_agent::CapabilityError;

/// Error taxonomy mirrored from [`CapabilityError`]; `Ok` carries no message.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClawCapabilityErrorKind {
    Ok = 0,
    InvalidArgument = 1,
    NotFound = 2,
    AlreadyExists = 3,
    InvalidState = 4,
    Failed = 5,
}

/// A Result value. `message` is `NULL` on `Ok` and a **borrowed** `const char*`
/// otherwise — the receiver copies it if needed and never frees it.
#[repr(C)]
pub struct ClawCapabilityResult {
    pub kind: ClawCapabilityErrorKind,
    pub message: *const c_char,
}

thread_local! {
    /// Holds the most recent dynamic `Failed` text so the returned pointer stays
    /// valid until the next claw-cabi call on this thread replaces it.
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

/// Success.
pub(crate) fn ok() -> ClawCapabilityResult {
    ClawCapabilityResult {
        kind: ClawCapabilityErrorKind::Ok,
        message: ptr::null(),
    }
}

/// A result whose message is a `'static` C string (structural errors, panic).
pub(crate) fn with_static(
    kind: ClawCapabilityErrorKind,
    message: &'static CStr,
) -> ClawCapabilityResult {
    ClawCapabilityResult {
        kind,
        message: message.as_ptr(),
    }
}

/// A `Failed` result whose message is owned by the thread-local buffer.
pub(crate) fn failed(message: &str) -> ClawCapabilityResult {
    // CString rejects interior NUL; strip them so we never lose the message.
    let sanitized = message.replace('\0', " ");
    let owned = CString::new(sanitized).unwrap_or_default();
    LAST_ERROR.with(|cell| {
        let mut slot = cell.borrow_mut();
        *slot = Some(owned);
        let pointer = slot
            .as_ref()
            .map(|value| value.as_ptr())
            .unwrap_or(ptr::null());
        ClawCapabilityResult {
            kind: ClawCapabilityErrorKind::Failed,
            message: pointer,
        }
    })
}

/// Map a `Result` from the Rust registry into the C result.
pub(crate) fn from_result(result: Result<(), CapabilityError>) -> ClawCapabilityResult {
    match result {
        Ok(()) => ok(),
        Err(error) => from_error(error),
    }
}

/// Map a [`CapabilityError`] into the C result.
pub(crate) fn from_error(error: CapabilityError) -> ClawCapabilityResult {
    match error {
        CapabilityError::InvalidArg => with_static(
            ClawCapabilityErrorKind::InvalidArgument,
            c"invalid argument",
        ),
        CapabilityError::NotFound => with_static(
            ClawCapabilityErrorKind::NotFound,
            c"capability or group not found",
        ),
        CapabilityError::AlreadyExists => with_static(
            ClawCapabilityErrorKind::AlreadyExists,
            c"capability or group already exists",
        ),
        CapabilityError::InvalidState => with_static(
            ClawCapabilityErrorKind::InvalidState,
            c"invalid state for this operation",
        ),
        CapabilityError::Failed(message) => failed(&message),
    }
}

/// Map a C result returned by a callback back into a Rust `Result`. The borrowed
/// `Failed` message is copied synchronously (and never freed).
///
/// # Safety
/// `result.message` must be null or a valid C string for the duration of this
/// call (the documented callback contract).
pub(crate) unsafe fn into_result(result: ClawCapabilityResult) -> Result<(), CapabilityError> {
    match result.kind {
        ClawCapabilityErrorKind::Ok => Ok(()),
        ClawCapabilityErrorKind::InvalidArgument => Err(CapabilityError::InvalidArg),
        ClawCapabilityErrorKind::NotFound => Err(CapabilityError::NotFound),
        ClawCapabilityErrorKind::AlreadyExists => Err(CapabilityError::AlreadyExists),
        ClawCapabilityErrorKind::InvalidState => Err(CapabilityError::InvalidState),
        ClawCapabilityErrorKind::Failed => {
            let message = if result.message.is_null() {
                String::from("capability callback failed")
            } else {
                CStr::from_ptr(result.message)
                    .to_string_lossy()
                    .into_owned()
            };
            Err(CapabilityError::Failed(message))
        }
    }
}

/// Run an FFI body, converting any panic into `Failed` so unwinding never
/// crosses into C.
pub(crate) fn guard(body: impl FnOnce() -> ClawCapabilityResult) -> ClawCapabilityResult {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(result) => result,
        Err(_) => with_static(
            ClawCapabilityErrorKind::Failed,
            c"panic caught at the claw-cabi boundary",
        ),
    }
}
