//! Capability-layer errors.

/// Failure outcome of capability registration and lifecycle.
///
/// `Failed(_)` is the only stringly variant, reserved for the pluggable boundary
/// the registry cannot classify: a [`Lifecycle`](crate::Lifecycle) /
/// [`ChannelAdapter`](crate::ChannelAdapter) hook reporting its own failure.
///
/// Tool *invocation* failures are **not** modeled here: a capability with the
/// [`Tool`](crate::CapabilityRole::Tool) role is a [`claw_tool::Tool`], so its
/// argument/handler errors are [`claw_tool::ToolInvokeError`].
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum CapabilityError {
    #[error("invalid argument")]
    InvalidArg,
    #[error("capability or group not found")]
    NotFound,
    #[error("capability or group already exists")]
    AlreadyExists,
    #[error("invalid state for this operation")]
    InvalidState,
    /// A `Lifecycle` or `ChannelAdapter` hook reported its own failure. The
    /// registry forwards the message unchanged; it does not interpret it.
    #[error("capability operation failed: {0}")]
    Failed(String),
    /// Loading or persisting enable/disable state through the injected
    /// [`CapabilityStateStore`](crate::CapabilityStateStore) failed (an IO or
    /// (de)serialization error). The message is forwarded unchanged.
    #[error("capability state persistence failed: {0}")]
    Persistence(String),
}
