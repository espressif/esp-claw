mod channel;
mod registry;
mod sink;
mod types;

pub use channel::{Channel, ChannelHandler, ChannelRuntime};
pub use registry::{ChannelRegistry, ChannelRegistryError};
pub use sink::ChannelSink;
pub use types::{
    ChannelAttachment, ChannelAttachmentKind, ChannelError, ChannelInbound, ChannelName,
    ChannelOutbound, ChannelResult, ChannelTarget, ChannelTargetOwned,
};

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use std::sync::atomic::{AtomicU32, Ordering};
    use std::sync::{Arc, Mutex};

    use super::{
        Channel, ChannelHandler, ChannelInbound, ChannelOutbound, ChannelRegistry,
        ChannelRegistryError, ChannelResult, ChannelRuntime, ChannelSink, ChannelTarget,
    };

    #[test]
    fn registry_starts_receives_sends_and_restarts_channel() -> Result<(), ChannelRegistryError> {
        let inbound = Arc::new(Mutex::new(Vec::new()));
        let inbound_sink = Arc::clone(&inbound);
        let sink = ChannelSink::new(move |input| {
            inbound_sink.lock().unwrap().push(input);
            Ok(())
        });

        let started = Arc::new(AtomicU32::new(0));
        let stopped = Arc::new(AtomicU32::new(0));
        let sent = Arc::new(Mutex::new(Vec::new()));
        let registry = ChannelRegistry::new(sink);

        registry.register(Channel::from_handler(TestChannel {
            started: Arc::clone(&started),
            stopped: Arc::clone(&stopped),
            sent: Arc::clone(&sent),
        }))?;

        assert_eq!(started.load(Ordering::SeqCst), 1);
        assert_eq!(inbound.lock().unwrap().len(), 1);

        registry.send(outbound("hello"))?;
        assert_eq!(sent.lock().unwrap().as_slice(), ["hello"]);

        registry.stop_all()?;
        assert_eq!(stopped.load(Ordering::SeqCst), 1);
        assert!(matches!(
            registry.send(outbound("after stop")),
            Err(ChannelRegistryError::SendFailed(name, _)) if name == "test"
        ));

        registry.start_all()?;
        assert_eq!(started.load(Ordering::SeqCst), 2);
        registry.send(outbound("after start"))?;
        assert_eq!(sent.lock().unwrap().as_slice(), ["hello", "after start"]);
        Ok(())
    }

    struct TestChannel {
        started: Arc<AtomicU32>,
        stopped: Arc<AtomicU32>,
        sent: Arc<Mutex<Vec<String>>>,
    }

    impl ChannelHandler for TestChannel {
        fn name(&self) -> &str {
            "test"
        }

        fn start(&self, sink: ChannelSink) -> ChannelResult<ChannelRuntime> {
            self.started.fetch_add(1, Ordering::SeqCst);
            sink.submit(ChannelInbound {
                channel: "test".into(),
                chat_id: "chat".into(),
                text: Some("inbound".into()),
                attachments: Vec::new(),
                sender_id: None,
                message_id: None,
                correlation_id: None,
                timestamp_ms: None,
                target: None,
                content_type: None,
                payload_json: None,
            })?;
            let stopped = Arc::clone(&self.stopped);
            Ok(ChannelRuntime::new(move || {
                stopped.fetch_add(1, Ordering::SeqCst);
                Ok(())
            }))
        }

        fn send(&self, message: ChannelOutbound<'_>) -> ChannelResult<()> {
            if let Some(text) = message.text {
                self.sent.lock().unwrap().push(text.to_owned());
            }
            Ok(())
        }
    }

    fn outbound(text: &str) -> ChannelOutbound<'_> {
        ChannelOutbound {
            target: ChannelTarget {
                channel: "test",
                chat_id: "chat",
            },
            text: Some(text),
            attachments: &[],
            message_id: None,
            correlation_id: None,
            payload_json: None,
        }
    }
}
