use std::fmt;
use std::sync::Arc;

use super::types::{ChannelError, ChannelInbound, ChannelResult};

#[derive(Clone)]
pub struct ChannelSink {
    submit: Arc<dyn Fn(ChannelInbound) -> ChannelResult<()> + Send + Sync>,
}

impl ChannelSink {
    pub fn new(
        submit: impl Fn(ChannelInbound) -> ChannelResult<()> + Send + Sync + 'static,
    ) -> Self {
        Self {
            submit: Arc::new(submit),
        }
    }

    pub fn submit(&self, input: ChannelInbound) -> ChannelResult<()> {
        (self.submit)(input)
    }
}

impl Default for ChannelSink {
    fn default() -> Self {
        Self::new(|_| Err(ChannelError::new("channel sink is closed")))
    }
}

impl fmt::Debug for ChannelSink {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ChannelSink")
            .finish_non_exhaustive()
    }
}
