//! `claw-sandbox` — the sandbox filesystem.
//!
//! This crate is the sandbox the agent runs in: it confines file access to a
//! fixed set of virtual roots — `/sandbox` (private, ephemeral), `/shared`
//! (shared with the host, persistent), and `/system` (system-provided,
//! read-only). Only the explicitly allowed paths are visible; any path outside
//! them is rejected. Backed by an injected [`ClawFs`], the same confinement
//! applies on-device and in host tests.
//!
//! [`ClawFs`]: claw_interface::ClawFs

pub mod fs;
pub mod sandbox;

pub use fs::{SandboxError, SandboxFs, READ_ONLY_PREFIXES, VISIBLE_PREFIXES};
pub use sandbox::{RealRoots, Sandbox};
