//! Byte stream backends for common targets.

#[cfg(all(feature = "espidf", target_os = "espidf"))]
mod espidf;
#[cfg(feature = "stdio")]
mod stdio;

#[cfg(all(feature = "espidf", target_os = "espidf"))]
pub use espidf::EspIdf;
#[cfg(feature = "stdio")]
pub use stdio::Stdio;
