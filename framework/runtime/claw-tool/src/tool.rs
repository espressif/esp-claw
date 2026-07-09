use core::future::Future;
use core::pin::Pin;
use std::fmt;
use std::sync::Arc;

use claw_permission::{Action, RiskClass};

use super::validate;

pub type ToolFuture<'a> = Pin<Box<dyn Future<Output = ToolResult<ToolOutput>> + Send + 'a>>;
pub type ToolResult<T> = Result<T, ToolInvokeError>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RawToolInvocation<'a> {
    pub id: Option<&'a str>,
    pub name: &'a str,
    pub arguments_json: &'a str,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ToolInvocation<'a> {
    id: Option<&'a str>,
    name: &'a str,
    arguments_json: &'a str,
}

impl<'a> TryFrom<RawToolInvocation<'a>> for ToolInvocation<'a> {
    type Error = ToolInvokeError;

    fn try_from(raw: RawToolInvocation<'a>) -> Result<Self, Self::Error> {
        let arguments_json = validate::normalize_arguments_json(raw.arguments_json)?;
        Ok(Self {
            id: raw.id,
            name: raw.name,
            arguments_json,
        })
    }
}

impl<'a> ToolInvocation<'a> {
    pub fn id(&self) -> Option<&str> {
        self.id
    }

    pub fn name(&self) -> &str {
        self.name
    }

    pub fn arguments_json(&self) -> &str {
        self.arguments_json
    }

    pub fn arguments_value(&self) -> ToolResult<serde_json::Value> {
        validate::parse_arguments_json(self.arguments_json)
    }
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
}

impl ToolInvokeError {
    pub fn new(error: ToolError) -> Self {
        Self { error }
    }
}

impl fmt::Display for ToolInvokeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.error.fmt(formatter)
    }
}

impl std::error::Error for ToolInvokeError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        Some(&self.error)
    }
}

impl From<ToolError> for ToolInvokeError {
    fn from(error: ToolError) -> Self {
        Self::new(error)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RetryCount {
    extra_attempts: u32,
}

impl RetryCount {
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

    fn retry_count(&self) -> RetryCount {
        RetryCount::none()
    }

    fn classify(&self, _call: &ToolInvocation<'_>) -> Action {
        Action::new(self.name(), RiskClass::High)
    }
}

pub trait SyncToolHandler: ToolSpec {
    fn invoke(&self, call: &ToolInvocation<'_>) -> ToolResult<ToolOutput>;
}

pub trait AsyncToolHandler: ToolSpec {
    fn invoke<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolFuture<'a>;
}

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
        let mut remaining = self.spec().retry_count().extra_attempts();
        loop {
            match self.invoke_once(call).await {
                Ok(output) => return Ok(output),
                Err(_) if remaining > 0 => {
                    remaining = remaining.saturating_sub(1);
                }
                Err(error) => return Err(error),
            }
        }
    }

    async fn invoke_once<'a>(&'a self, call: &'a ToolInvocation<'_>) -> ToolResult<ToolOutput> {
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
