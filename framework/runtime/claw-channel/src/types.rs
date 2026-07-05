use std::fmt;

pub type ChannelName = String;
pub type ChannelResult<T> = Result<T, ChannelError>;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChannelInbound {
    pub channel: ChannelName,
    pub chat_id: String,
    pub text: Option<String>,
    pub attachments: Vec<ChannelAttachment>,
    pub sender_id: Option<String>,
    pub message_id: Option<String>,
    pub correlation_id: Option<String>,
    pub timestamp_ms: Option<i64>,
    pub target: Option<ChannelTargetOwned>,
    pub content_type: Option<String>,
    pub payload_json: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChannelTarget<'a> {
    pub channel: &'a str,
    pub chat_id: &'a str,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChannelTargetOwned {
    pub channel: String,
    pub chat_id: String,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChannelOutbound<'a> {
    pub target: ChannelTarget<'a>,
    pub text: Option<&'a str>,
    pub attachments: &'a [ChannelAttachment],
    pub message_id: Option<&'a str>,
    pub correlation_id: Option<&'a str>,
    pub payload_json: Option<&'a str>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChannelAttachment {
    pub kind: ChannelAttachmentKind,
    pub path: Option<String>,
    pub url: Option<String>,
    pub name: Option<String>,
    pub mime_type: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelAttachmentKind {
    Image,
    File,
    Link,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChannelError {
    message: String,
}

impl ChannelError {
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for ChannelError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for ChannelError {}
