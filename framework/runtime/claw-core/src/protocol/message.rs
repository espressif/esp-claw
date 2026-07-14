//! Messages delivered to sessions and agents.

use serde::{Deserialize, Deserializer, Serialize};

use super::{AgentId, TurnOrigin};

/// One message delivered to a session or agent.
///
#[derive(Clone, Debug, Deserialize, PartialEq, Eq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Message {
    text: String,
    #[serde(default, skip_serializing_if = "TurnOrigin::is_user")]
    origin: TurnOrigin,
}

impl Message {
    /// Build a text message.
    pub fn text(text: impl Into<String>) -> Self {
        Self {
            text: text.into(),
            origin: TurnOrigin::User,
        }
    }

    pub(crate) fn as_str(&self) -> &str {
        &self.text
    }

    pub(crate) fn from_subagent(agent: AgentId, text: impl Into<String>) -> Self {
        Self {
            text: text.into(),
            origin: TurnOrigin::Subagent { agent },
        }
    }

    pub(crate) fn origin(&self) -> TurnOrigin {
        self.origin
    }

    pub(crate) fn into_user(mut self) -> Self {
        self.origin = TurnOrigin::User;
        self
    }
}

/// Decode the canonical message object or the legacy plain string used by
/// persisted agent mailboxes before task inputs retained their `Message` type.
pub(crate) fn deserialize_message_or_text<'de, D>(deserializer: D) -> Result<Message, D::Error>
where
    D: Deserializer<'de>,
{
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Repr {
        Message(Message),
        Text(String),
    }

    Ok(match Repr::deserialize(deserializer)? {
        Repr::Message(message) => message,
        Repr::Text(text) => Message::text(text),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn text_message_projects_to_plain_text() {
        let message = Message::text("hello");

        assert_eq!(message.as_str(), "hello");
    }

    #[test]
    fn durable_shape_contains_only_current_text_input() {
        let encoded = serde_json::to_value(Message::text("hello")).expect("message encodes");

        assert_eq!(encoded, serde_json::json!({ "text": "hello" }));
        assert!(serde_json::from_value::<Message>(serde_json::json!({
            "text": "hello",
            "attachments": []
        }))
        .is_err());
    }

    #[test]
    fn subagent_origin_survives_mailbox_persistence() {
        let message = Message::from_subagent(AgentId(7), "done");
        let encoded = serde_json::to_value(&message).expect("message encodes");

        assert_eq!(
            encoded,
            serde_json::json!({
                "text": "done",
                "origin": { "type": "subagent", "agent": "agent-7" },
            })
        );
        let restored: Message = serde_json::from_value(encoded).expect("message decodes");
        assert_eq!(
            restored.origin(),
            TurnOrigin::Subagent { agent: AgentId(7) }
        );
    }
}
