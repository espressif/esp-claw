use std::fmt;
use std::sync::{Arc, Mutex, PoisonError};

use super::sink::ChannelSink;
use super::types::{ChannelOutbound, ChannelResult};

pub trait ChannelHandler: Send + Sync {
    fn name(&self) -> &str;

    fn start(&self, sink: ChannelSink) -> ChannelResult<ChannelRuntime>;

    fn send(&self, message: ChannelOutbound<'_>) -> ChannelResult<()>;
}

#[derive(Clone)]
pub struct Channel {
    inner: Arc<dyn ChannelHandler>,
}

impl Channel {
    pub fn from_handler(handler: impl ChannelHandler + 'static) -> Self {
        Self {
            inner: Arc::new(handler),
        }
    }

    pub fn name(&self) -> &str {
        self.inner.name()
    }

    pub fn start(&self, sink: ChannelSink) -> ChannelResult<ChannelRuntime> {
        self.inner.start(sink)
    }

    pub fn send(&self, message: ChannelOutbound<'_>) -> ChannelResult<()> {
        self.inner.send(message)
    }
}

impl fmt::Debug for Channel {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("Channel")
            .field(&self.name())
            .finish()
    }
}

pub struct ChannelRuntime {
    stop: Mutex<Option<Box<dyn FnOnce() -> ChannelResult<()> + Send>>>,
}

impl ChannelRuntime {
    pub fn new(stop: impl FnOnce() -> ChannelResult<()> + Send + 'static) -> Self {
        Self {
            stop: Mutex::new(Some(Box::new(stop))),
        }
    }

    pub fn stop(&self) -> ChannelResult<()> {
        let stop = {
            let mut stop = self.stop.lock().unwrap_or_else(PoisonError::into_inner);
            stop.take()
        };
        let Some(stop) = stop else {
            return Ok(());
        };
        stop()
    }
}

impl Default for ChannelRuntime {
    fn default() -> Self {
        Self::new(|| Ok(()))
    }
}

impl fmt::Debug for ChannelRuntime {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let stop = self.stop.lock().unwrap_or_else(PoisonError::into_inner);
        formatter
            .debug_struct("ChannelRuntime")
            .field("started", &stop.is_some())
            .finish()
    }
}
