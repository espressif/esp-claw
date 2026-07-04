use std::collections::HashMap;
use std::fmt;
use std::sync::{PoisonError, RwLock, RwLockReadGuard, RwLockWriteGuard};

use super::channel::{Channel, ChannelRuntime};
use super::sink::ChannelSink;
use super::types::{ChannelError, ChannelName, ChannelOutbound};

#[derive(Default)]
pub struct ChannelRegistry {
    channels: RwLock<HashMap<ChannelName, ChannelEntry>>,
    sink: ChannelSink,
}

struct ChannelEntry {
    channel: Channel,
    runtime: Option<ChannelRuntime>,
}

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum ChannelRegistryError {
    #[error("channel already exists: {0}")]
    AlreadyExists(ChannelName),
    #[error("channel not found: {0}")]
    NotFound(ChannelName),
    #[error("invalid channel: {0}")]
    InvalidChannel(ChannelName),
    #[error("channel start failed: {0}: {1}")]
    StartFailed(ChannelName, ChannelError),
    #[error("channel send failed: {0}: {1}")]
    SendFailed(ChannelName, ChannelError),
    #[error("channel stop failed: {0}: {1}")]
    StopFailed(ChannelName, ChannelError),
}

impl ChannelRegistry {
    pub fn new(sink: ChannelSink) -> Self {
        Self {
            channels: RwLock::new(HashMap::new()),
            sink,
        }
    }

    pub fn register(&self, channel: Channel) -> Result<(), ChannelRegistryError> {
        let name = channel.name().to_owned();
        if name.is_empty() {
            return Err(ChannelRegistryError::InvalidChannel(name));
        }

        let mut channels = self.write_channels();
        if channels.contains_key(&name) {
            return Err(ChannelRegistryError::AlreadyExists(name));
        }

        let runtime = channel
            .start(self.sink.clone())
            .map_err(|error| ChannelRegistryError::StartFailed(name.clone(), error))?;
        channels.insert(
            name,
            ChannelEntry {
                channel,
                runtime: Some(runtime),
            },
        );
        Ok(())
    }

    pub fn start_all(&self) -> Result<(), ChannelRegistryError> {
        let mut channels = self.write_channels();
        for (name, entry) in channels.iter_mut() {
            if entry.runtime.is_some() {
                continue;
            }
            let runtime = entry
                .channel
                .start(self.sink.clone())
                .map_err(|error| ChannelRegistryError::StartFailed(name.clone(), error))?;
            entry.runtime = Some(runtime);
        }
        Ok(())
    }

    pub fn send(&self, message: ChannelOutbound<'_>) -> Result<(), ChannelRegistryError> {
        let name = message.target.channel.to_owned();
        let channels = self.read_channels();
        let Some(entry) = channels.get(message.target.channel) else {
            return Err(ChannelRegistryError::NotFound(name));
        };
        if entry.runtime.is_none() {
            return Err(ChannelRegistryError::SendFailed(
                name,
                ChannelError::new("channel is stopped"),
            ));
        }
        entry
            .channel
            .send(message)
            .map_err(|error| ChannelRegistryError::SendFailed(name, error))
    }

    pub fn stop_all(&self) -> Result<(), ChannelRegistryError> {
        let mut channels = self.write_channels();
        for (name, entry) in channels.iter_mut() {
            let Some(runtime) = entry.runtime.as_ref() else {
                continue;
            };
            runtime
                .stop()
                .map_err(|error| ChannelRegistryError::StopFailed(name.clone(), error))?;
            entry.runtime = None;
        }
        Ok(())
    }

    fn read_channels(&self) -> RwLockReadGuard<'_, HashMap<ChannelName, ChannelEntry>> {
        self.channels.read().unwrap_or_else(PoisonError::into_inner)
    }

    fn write_channels(&self) -> RwLockWriteGuard<'_, HashMap<ChannelName, ChannelEntry>> {
        self.channels
            .write()
            .unwrap_or_else(PoisonError::into_inner)
    }
}

impl fmt::Debug for ChannelRegistry {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let channels = self.read_channels();
        let started = channels
            .values()
            .filter(|entry| entry.runtime.is_some())
            .count();
        formatter
            .debug_struct("ChannelRegistry")
            .field("channels", &channels.len())
            .field("started", &started)
            .finish()
    }
}
