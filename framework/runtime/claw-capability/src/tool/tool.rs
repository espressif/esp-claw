use core::future::Future;
use core::pin::Pin;
use std::fmt;
use std::sync::Arc;

use claw_permission::{Action, RiskClass};

pub type ToolFuture<'a> = Pin<Box<dyn Future<Output = ToolResult<ToolOutput>> + Send + 'a>>;
pub type ToolResult<T> = Result<T, ToolInvokeError>;

#[derive(Clone, Debug, PartialEq, Eq)]
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

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum ToolError {
    #[error("tool not found: {0}")]
    NotFound(String),
    #[error("invalid arguments json: {0}")]
    InvalidArgumentsJson(String),
    #[error("invalid arguments: {0}")]
    InvalidArguments(String),
    #[error("tool invocation rejected: {0}")]
    InvokeRejected(String),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ToolInvokeError {
    pub error: ToolError,
    pub retries: ToolRetryCount,
}

impl ToolInvokeError {
    pub fn new(error: ToolError) -> Self {
        Self {
            error,
            retries: ToolRetryCount::none(),
        }
    }

    pub fn with_retries(error: ToolError, retries: ToolRetryCount) -> Self {
        Self { error, retries }
    }
}

impl From<ToolError> for ToolInvokeError {
    fn from(error: ToolError) -> Self {
        Self::new(error)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ToolRetryCount {
    extra_attempts: u32,
}

impl ToolRetryCount {
    pub fn none() -> Self {
        Self { extra_attempts: 0 }
    }

    pub fn extra(extra_attempts: u32) -> Self {
        Self { extra_attempts }
    }

    pub fn extra_attempts(self) -> u32 {
        self.extra_attempts
    }
}

pub trait ToolSpec: Send + Sync {
    fn name(&self) -> &str;

    fn schema(&self) -> &str;

    fn usage(&self) -> Option<&str> {
        None
    }

    fn concurrent(&self) -> bool {
        false
    }

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new(self.name(), RiskClass::Safe)
    }
}

pub trait SyncToolHandler: ToolSpec {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput>;
}

pub trait AsyncToolHandler: ToolSpec {
    fn invoke<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a>;
}

#[derive(Clone)]
pub struct Tool {
    inner: Arc<ToolInner>,
}

enum ToolInner {
    Sync(Box<dyn SyncToolHandler>),
    Async(Box<dyn AsyncToolHandler>),
}

impl Tool {
    pub fn from_sync(handler: impl SyncToolHandler + 'static) -> Self {
        Self {
            inner: Arc::new(ToolInner::Sync(Box::new(handler))),
        }
    }

    pub fn from_async(handler: impl AsyncToolHandler + 'static) -> Self {
        Self {
            inner: Arc::new(ToolInner::Async(Box::new(handler))),
        }
    }

    pub fn name(&self) -> &str {
        self.spec().name()
    }

    pub fn schema(&self) -> &str {
        self.spec().schema()
    }

    pub fn usage(&self) -> Option<&str> {
        self.spec().usage()
    }

    pub(crate) fn classify(&self, call: &ToolInvocation<'_>) -> Action {
        self.spec().classify(call)
    }

    pub(crate) async fn invoke<'a>(
        &'a self,
        call: &'a ToolInvocation<'_>,
    ) -> ToolResult<ToolOutput> {
        match self.inner.as_ref() {
            ToolInner::Sync(handler) => handler.invoke(call),
            ToolInner::Async(handler) => handler.invoke(call).await,
        }
    }

    fn spec(&self) -> &dyn ToolSpec {
        match self.inner.as_ref() {
            ToolInner::Sync(handler) => handler.as_ref(),
            ToolInner::Async(handler) => handler.as_ref(),
        }
    }
}

impl fmt::Debug for Tool {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_tuple("Tool").field(&self.name()).finish()
    }
}
