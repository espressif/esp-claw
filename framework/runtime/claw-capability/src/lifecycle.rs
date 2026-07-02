//! Capability lifecycle — an orthogonal concern, not a role.
//!
//! `init`/`start`/`stop` manage the resources a capability owns (transport
//! tasks, sockets, mDNS, an engine). It is *independent* of what the capability
//! exposes ([`CapabilityRole`](crate::CapabilityRole)): a `Tool` capability can
//! own a runtime, a `Channel` owns its transport, and a capability with no
//! invocation surface at all may exist *only* for its lifecycle (an MCP server).

use crate::error::CapabilityError;

/// Resource lifecycle attached to a capability or a group. All hooks default to
/// no-ops, so a capability that owns no resources implements nothing.
///
/// Two paired phases, run by the registry in this order over a capability's
/// life:
///
/// ```text
/// init  →  ( start → stop )*  →  deinit
/// ```
///
/// - `init` / `deinit` are the **one-time** pair: `init` runs at most once
///   (before the first `start`); `deinit` runs at most once (after the last
///   `stop`, when the capability is unregistered), and only if `init` ran.
/// - `start` / `stop` are the **per-activation** pair: they run on every
///   enable / disable cycle.
pub trait Lifecycle: Send + Sync {
    /// One-time initialization, run at most once before the first `start`.
    fn init(&self) -> Result<(), CapabilityError> {
        Ok(())
    }

    /// Acquire/begin serving. Run on every enable.
    fn start(&self) -> Result<(), CapabilityError> {
        Ok(())
    }

    /// Release/stop serving. Run on every disable (best-effort, reverse order).
    fn stop(&self) -> Result<(), CapabilityError> {
        Ok(())
    }

    /// One-time teardown, the counterpart to `init`. Run at most once when the
    /// capability is unregistered, after the final `stop`, and only if `init`
    /// previously ran. Best-effort, reverse order.
    fn deinit(&self) -> Result<(), CapabilityError> {
        Ok(())
    }
}

/// Lifecycle state of a registered group and its members.
///
/// A capability is `Registered` (known, not serving), `Started` (serving), or
/// `Disabled` (administratively off). There is no draining state: dispatch does
/// not happen in this layer (tools are invoked through `claw-tool`), so there is
/// nothing in flight to serialize against unregister.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum CapabilityState {
    /// Registered but not started (registry not started, or group disabled-then-registered).
    #[default]
    Registered,
    /// Started and serving.
    Started,
    /// Administratively disabled.
    Disabled,
}

impl CapabilityState {
    /// Lowercase label for logging and display.
    pub const fn as_str(self) -> &'static str {
        match self {
            CapabilityState::Registered => "registered",
            CapabilityState::Started => "started",
            CapabilityState::Disabled => "disabled",
        }
    }

    /// Available to list / hand out for the registry's current lifecycle phase.
    ///
    /// Before the registry is globally started, `Registered` roles remain visible
    /// so the host can wire transports during construction. Once started, only
    /// groups whose lifecycle reached `Started` are visible.
    pub(crate) fn is_available(self, registry_started: bool) -> bool {
        if registry_started {
            matches!(self, CapabilityState::Started)
        } else {
            matches!(self, CapabilityState::Registered | CapabilityState::Started)
        }
    }
}
