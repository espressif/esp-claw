//! Defining a tool: the [`ToolHandler`] / [`AsyncToolHandler`] traits, the
//! [`tool_metadata!`] macro that bakes its metadata from
//! `resources/tools/<name>/`, and the cheap-to-clone [`Tool`] value built from a
//! handler.

use core::future::Future;
use core::pin::Pin;
use std::num::NonZeroU32;
use std::sync::Arc;

use claw_permission::{Action, RiskClass};
use thiserror::Error;

/// Boxed future returned by [`AsyncToolHandler::invoke_async`].
///
/// The future borrows the handler and invocation, so it cannot outlive either.
/// It intentionally does not require `Send`: the agent/tool runner is designed
/// to be driven by a single cooperative task, and runtime boundaries that cross
/// threads can add stricter requirements there.
pub type ToolFuture<'a> = Pin<Box<dyn Future<Output = Result<ToolOutput, ToolInvokeError>> + 'a>>;

/// One model tool_call.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ToolInvocation<'a> {
    pub id: Option<&'a str>,
    pub name: &'a str,
    pub arguments_json: &'a str,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ToolOutput {
    pub output: String,
    pub ok: bool,
}

#[derive(Clone, Debug, PartialEq, Eq, Error)]
pub enum ToolError {
    #[error("tool not found: {0}")]
    NotFound(String),
    /// Arguments are not valid JSON, or the root value is not a JSON object.
    #[error("tool arguments are not valid JSON: {0}")]
    InvalidArgumentsJson(String),
    /// Arguments are a JSON object but failed tool-specific validation.
    #[error("tool arguments are invalid: {0}")]
    InvalidArguments(String),
    /// Dynamic / domain validation inside [`ToolHandler::invoke`] that simple
    /// argument shape checks cannot express (policy, wire-format ids,
    /// cross-field rules, etc.).
    #[error("tool invocation rejected: {0}")]
    InvokeRejected(String),
}

/// Per-call automatic retry budget for the same `tool_call` after a failure.
///
/// Use [`none`](Self::none) for no automatic retry, or [`extra`](Self::extra) with
/// how many **additional** invocations to allow after the first failure.
///
/// # Roadmap
///
/// Today the retry budget is honored by an immediate re-invoke loop (no backoff,
/// no preemption between attempts). It is the boundary the planned async,
/// fair-scheduling tool runner grows into — see the workspace ROADMAP
/// (`framework/runtime/ROADMAP.md`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct ToolRetryCount(Option<NonZeroU32>);

impl ToolRetryCount {
    /// No automatic re-invocation — surface the error to the model immediately.
    pub const fn none() -> Self {
        Self(None)
    }

    /// Allow `extra_attempts` more invocations after the first failure.
    ///
    /// `extra_attempts = 0` is the same as [`none`](Self::none); `1` retries once;
    /// `2` allows up to three invocations total.
    pub fn extra(extra_attempts: u32) -> Self {
        Self(NonZeroU32::new(extra_attempts))
    }

    pub fn is_none(self) -> bool {
        self.0.is_none()
    }

    /// Additional invocations permitted after the first failure; zero when empty.
    pub fn get(self) -> u32 {
        self.0.map(|count| count.get()).unwrap_or(0)
    }
}

/// A failed tool invocation: the [`ToolError`] plus a per-call automatic retry
/// budget. An empty [`retries`](Self::retries) budget means surface the error to
/// the model immediately; a non-empty one lets [`ToolRunner`](crate::ToolRunner)
/// re-invoke the same call before giving up.
///
/// Most handlers never construct this directly — returning a [`ToolError`] uses
/// the [`From`] impl (no retry). Reach for [`with_retries`](Self::with_retries)
/// only for a transient failure worth an automatic re-invoke.
#[derive(Clone, Debug, PartialEq, Eq, Error)]
#[error("{error}")]
pub struct ToolInvokeError {
    /// What went wrong.
    #[source]
    pub error: ToolError,
    /// How many times the runtime may re-invoke the same call before surfacing it.
    pub retries: ToolRetryCount,
}

impl ToolInvokeError {
    /// An immediate failure with no automatic re-invocation.
    pub fn new(error: impl Into<ToolError>) -> Self {
        Self {
            error: error.into(),
            retries: ToolRetryCount::none(),
        }
    }

    /// A failure paired with an explicit automatic retry budget.
    pub fn with_retries(error: impl Into<ToolError>, retries: ToolRetryCount) -> Self {
        Self {
            error: error.into(),
            retries,
        }
    }
}

impl From<ToolError> for ToolInvokeError {
    fn from(error: ToolError) -> Self {
        Self::new(error)
    }
}

impl ToolError {
    /// Build an [`InvokeRejected`] for dynamic validation inside `invoke`.
    pub fn invoke_rejected(message: impl Into<String>) -> Self {
        Self::InvokeRejected(message.into())
    }

    /// Model-facing text when the runner turns this error into a tool result
    /// (`ok = false`) so the model can self-correct.
    pub fn model_message(&self, tool_name: &str) -> String {
        let display = if tool_name.is_empty() {
            "(null)"
        } else {
            tool_name
        };
        match self {
            Self::NotFound(_) => {
                format!("Tool \"{display}\" is not registered and was not executed.")
            }
            Self::InvalidArgumentsJson(details) => format!(
                "Tool \"{display}\" arguments are not valid JSON: {details}. \
                 Fix the arguments and retry."
            ),
            Self::InvalidArguments(details) => format!(
                "Tool \"{display}\" arguments are invalid: {details}. \
                 Fix the arguments and retry."
            ),
            Self::InvokeRejected(details) => format!(
                "Tool \"{display}\" rejected the call: {details}. \
                 Fix the input and retry."
            ),
        }
    }
}

/// One synchronous model-callable tool: the full, concise shape a caller must
/// provide.
///
/// Implement this once per tool. A collection of tools is assembled into a
/// [`ToolSet`](crate::ToolSet) (which dispatches `invoke` by
/// [`name`](ToolHandler::name) and combines every [`schema`](ToolHandler::schema)
/// into the JSON sent to the LLM).
///
/// This trait is a permanent part of the API, not a compatibility layer. Use it
/// for immediate work, deterministic CPU-light parsing/formatting, and C-backed
/// capability callbacks. Async callers still drive sync handlers through
/// [`Tool::invoke_async`], which runs the handler body on the fixed tool
/// executor.
pub trait ToolHandler: Send + Sync {
    /// The tool's name. Must match the `name` field inside [`schema`](Self::schema)
    /// and what the model emits in its `tool_call`.
    ///
    /// Returns `&str` tied to `&self` — baked tools return a `&'static str` (which
    /// coerces), runtime-registered tools may return a field borrow.
    /// [`ToolSet`](crate::ToolSet) copies the name into an owned `String` key.
    fn name(&self) -> &str;

    /// This tool's OpenAI function schema as JSON text — one
    /// `{"type":"function", ...}` object.
    ///
    /// Returns `&str` tied to `&self` — typically a compile-time constant (string
    /// literal or `include_str!`). [`ToolSet`](crate::ToolSet) splices these texts
    /// into the tools array without building or serializing any
    /// `serde_json::Value`. The returned text must be a valid JSON object; this is
    /// checked with a `debug_assert!` when the set is built.
    fn schema(&self) -> &str;

    /// This tool's soft-tools prompt prose, or `None` when it has none.
    ///
    /// Baked from `resources/tools/<name>/usage.md` (a blank file ⇒ `None`). This
    /// is the *prompt* surface — guidance the model reads — as opposed to
    /// [`schema`](Self::schema), the *API* surface. The default is `None` for
    /// hand-written handlers that opt out; the [`tool_metadata!`] macro overrides
    /// it from the baked file. The aggregate stitches every non-empty value into
    /// the tool-policy prompt block (assembled by `claw-context`).
    fn usage(&self) -> Option<&str> {
        None
    }

    /// Whether this tool is safe to run *concurrently* with other tool calls in
    /// the same batch — true only when it has no observable side effects that
    /// could interleave badly (e.g. `web_search`, a pure read). The default is
    /// `false` (serialize), the safe choice: a tool that mutates shared state
    /// (e.g. `write_file` to the same path) must not race another call.
    fn concurrent(&self) -> bool {
        false
    }

    /// Describe what *this specific call* does as a permission [`Action`] (verb +
    /// optional resource + risk). The runtime evaluates it through the permission
    /// policy before invoking. The default is a [`Safe`](RiskClass::Safe) action
    /// keyed on the tool name; tools with side effects override this to raise the
    /// risk and name the resource they touch.
    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new(self.name(), RiskClass::Safe)
    }

    /// Execute one model `tool_call` addressed to this tool.
    fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError>;
}

/// Async model-callable tool implemented in Rust.
///
/// This trait is the Rust-side async surface for tools that await I/O or other
/// cooperative work. It is intentionally separate from [`ToolHandler`], not a
/// replacement for it. The metadata/classification methods mirror
/// [`ToolHandler`] so a [`Tool`] can hide whether the implementation is sync or
/// async from the aggregate/runner layers.
pub trait AsyncToolHandler: Send + Sync {
    /// The tool's name. Must match the `name` field inside [`schema`](Self::schema)
    /// and what the model emits in its `tool_call`.
    fn name(&self) -> &str;

    /// This tool's OpenAI function schema as JSON text.
    fn schema(&self) -> &str;

    /// This tool's soft-tools prompt prose, or `None` when it has none.
    fn usage(&self) -> Option<&str> {
        None
    }

    /// Whether this tool is safe to run concurrently with other calls in a batch.
    fn concurrent(&self) -> bool {
        false
    }

    /// Describe what this specific call does as a permission [`Action`].
    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new(self.name(), RiskClass::Safe)
    }

    /// Execute one model `tool_call` addressed to this tool.
    fn invoke_async<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a>;
}

/// Generate the `name()`, `schema()`, and `usage()` methods of a [`ToolHandler`]
/// or [`AsyncToolHandler`] impl from the tool's baked directory
/// `resources/tools/<name>/`.
///
/// `name()` returns the given literal; `schema()` embeds, at compile time,
/// `resources/tools/<name>/schema.json`; `usage()` embeds
/// `resources/tools/<name>/usage.md` (a blank file ⇒ `None`). All paths resolve
/// against **the using crate's** `CARGO_MANIFEST_DIR` (where the macro is
/// invoked), and stay `&'static str` constants — no runtime cost. Only
/// [`invoke`](ToolHandler::invoke) is then written by hand. The build step
/// ([`bake`](crate::bake)) enforces that the directory holds exactly these two
/// files and that the directory name equals the schema's `function.name`.
#[macro_export]
macro_rules! tool_metadata {
    ($name:literal) => {
        fn name(&self) -> &str {
            $name
        }

        fn schema(&self) -> &str {
            include_str!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/resources/tools/",
                $name,
                "/schema.json"
            ))
        }

        fn usage(&self) -> ::std::option::Option<&str> {
            const USAGE: &str = include_str!(concat!(
                env!("CARGO_MANIFEST_DIR"),
                "/resources/tools/",
                $name,
                "/usage.md"
            ));
            if USAGE.trim().is_empty() {
                ::std::option::Option::None
            } else {
                ::std::option::Option::Some(USAGE)
            }
        }
    };
}

/// A single tool as a cheap-to-clone value.
///
/// The handler lives behind an `Arc`, so cloning a `Tool` is a reference-count
/// bump — callers pass `Tool` by value (e.g. in an array) without paying for the
/// underlying implementation:
///
/// ```ignore
/// agent.with_tools([Tool::new(EchoTool), Tool::new(WeatherTool)]);
/// ```
#[derive(Clone)]
pub struct Tool {
    handler: ToolKind,
}

#[derive(Clone)]
enum ToolKind {
    Sync(Arc<dyn ToolHandler>),
    Async(Arc<dyn AsyncToolHandler>),
}

impl Tool {
    /// Wrap a [`ToolHandler`] into a shareable `Tool` value.
    pub fn new(handler: impl ToolHandler + 'static) -> Self {
        Self {
            handler: ToolKind::Sync(Arc::new(handler)),
        }
    }

    /// Wrap an [`AsyncToolHandler`] into a shareable `Tool` value.
    pub fn new_async(handler: impl AsyncToolHandler + 'static) -> Self {
        Self {
            handler: ToolKind::Async(Arc::new(handler)),
        }
    }

    /// The tool's name (delegates to the handler).
    pub fn name(&self) -> &str {
        match &self.handler {
            ToolKind::Sync(handler) => handler.name(),
            ToolKind::Async(handler) => handler.name(),
        }
    }

    /// The tool's function schema as JSON text (delegates to the handler).
    pub fn schema(&self) -> &str {
        match &self.handler {
            ToolKind::Sync(handler) => handler.schema(),
            ToolKind::Async(handler) => handler.schema(),
        }
    }

    /// The tool's soft-tools prompt prose, if any (delegates to the handler).
    pub fn usage(&self) -> Option<&str> {
        match &self.handler {
            ToolKind::Sync(handler) => handler.usage(),
            ToolKind::Async(handler) => handler.usage(),
        }
    }

    /// Whether this tool may run concurrently in a batch (delegates to the handler).
    pub fn concurrent(&self) -> bool {
        match &self.handler {
            ToolKind::Sync(handler) => handler.concurrent(),
            ToolKind::Async(handler) => handler.concurrent(),
        }
    }

    /// The permission [`Action`] this call represents (delegates to the handler).
    pub fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        match &self.handler {
            ToolKind::Sync(handler) => handler.classify(call),
            ToolKind::Async(handler) => handler.classify(call),
        }
    }

    /// Execute one `tool_call` through the synchronous surface.
    ///
    /// Async-only tools cannot be driven safely from this path: blocking on an
    /// async future here would defeat the cooperative runner and can deadlock if
    /// the future needs executor progress. Use [`invoke_async`](Self::invoke_async)
    /// for tools created with [`new_async`](Self::new_async).
    pub fn invoke(&self, call: &ToolInvocation<'_>) -> Result<ToolOutput, ToolInvokeError> {
        match &self.handler {
            ToolKind::Sync(handler) => handler.invoke(call),
            ToolKind::Async(_) => {
                Err(ToolError::invoke_rejected("async tool requires the async tool runner").into())
            }
        }
    }

    /// Execute one `tool_call` through the async surface.
    ///
    /// The concrete handler body runs on the fixed tool executor, so a sync/C
    /// handler cannot block the main agent executor while the caller awaits this
    /// future. The handler future itself is created on that worker and therefore
    /// does not need to be `Send`.
    pub fn invoke_async<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a> {
        crate::executor::invoke_on_global_executor(self.clone(), call)
    }

    pub(crate) fn invoke_inline_async<'a>(
        &'a self,
        call: &'a ToolInvocation<'_>,
    ) -> ToolFuture<'a> {
        match &self.handler {
            ToolKind::Sync(handler) => Box::pin(async move { handler.invoke(call) }),
            ToolKind::Async(handler) => handler.invoke_async(call),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug)]
    struct ExternalToolError;

    impl From<ExternalToolError> for ToolError {
        fn from(_: ExternalToolError) -> Self {
            ToolError::invoke_rejected("external")
        }
    }

    #[test]
    fn invoke_error_constructors_accept_convertible_errors() {
        let immediate = ToolInvokeError::new(ExternalToolError);
        assert_eq!(immediate.retries, ToolRetryCount::none());
        assert!(matches!(
            immediate.error,
            ToolError::InvokeRejected(message) if message == "external"
        ));

        let retried = ToolInvokeError::with_retries(ExternalToolError, ToolRetryCount::extra(2));
        assert_eq!(retried.retries, ToolRetryCount::extra(2));
        assert!(matches!(
            retried.error,
            ToolError::InvokeRejected(message) if message == "external"
        ));
    }
}
