//! Inbound user messages and attachment references.

use std::fmt::Write as _;

crate::define_prefixed_id!(AttachmentId, "att-", "attachment");

/// One inbound user message delivered to a session.
///
/// The text body is the normal chat message body. Attachments are references to
/// session attachment records; the record store is the source of truth for
/// non-text bytes and metadata.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Message {
    /// User-visible text body, including normalized captions when a transport
    /// sends an image/file with a caption.
    pub text: Option<String>,
    /// Attachment records associated with this user message.
    pub attachments: Vec<AttachmentRef>,
}

impl Message {
    /// Build a text-only message.
    pub fn text(text: impl Into<String>) -> Self {
        Self {
            text: Some(text.into()),
            attachments: Vec::new(),
        }
    }

    /// Render the current typed message into the existing text transcript lane.
    ///
    /// This is a temporary compatibility projection while the lower agent and
    /// transcript layers are still text-only.
    pub(crate) fn into_transcript_text(self) -> String {
        let mut rendered = self.text.unwrap_or_default();
        if self.attachments.is_empty() {
            return rendered;
        }
        if !rendered.is_empty() {
            rendered.push_str("\n\n");
        }
        rendered.push_str("[attachments]");
        for attachment in self.attachments {
            let _ = write!(rendered, "\n- id: {}", attachment.id);
        }
        rendered
    }
}

impl From<String> for Message {
    fn from(text: String) -> Self {
        Self::text(text)
    }
}

impl From<&str> for Message {
    fn from(text: &str) -> Self {
        Self::text(text)
    }
}

/// A reference from a chat message to an attachment stored in the session
/// manifest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttachmentRef {
    pub id: AttachmentId,
}

/// Normalized attachment kind derived from MIME at ingress.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum AttachmentKind {
    /// Derived from `image/<subtype>`.
    Image { subtype: String },
    /// Derived from `text/<subtype>` or a configured text-like MIME.
    Text { subtype: String },
    /// Any MIME that is not image/text-safe for this runtime.
    Unknown { mime: String },
}

/// Session manifest record for one saved attachment object.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttachmentRecord {
    pub id: AttachmentId,
    pub kind: AttachmentKind,
    pub path: String,
    pub name: Option<String>,
    pub size_bytes: Option<u64>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_message_projects_to_plain_text() {
        let message = Message::text("hello");

        assert_eq!(message.into_transcript_text(), "hello");
    }

    #[test]
    fn attachment_message_projects_attachment_ids() {
        let message = Message {
            text: Some("look".to_string()),
            attachments: vec![AttachmentRef {
                id: AttachmentId(3),
            }],
        };

        assert_eq!(
            message.into_transcript_text(),
            "look\n\n[attachments]\n- id: att-3"
        );
    }
}
