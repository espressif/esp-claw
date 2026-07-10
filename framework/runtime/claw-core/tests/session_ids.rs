#![allow(clippy::unwrap_used)]

use claw_core::{SessionId, TurnId};
use serde_json::json;

#[test]
fn session_id_serializes_to_prefixed_string() {
    let value = serde_json::to_value(SessionId(1)).unwrap();
    assert_eq!(value, json!("session-1"));
}

#[test]
fn session_id_deserializes_from_prefixed_string() {
    let session: SessionId = serde_json::from_value(json!("session-7")).unwrap();
    assert_eq!(session, SessionId(7));
}

#[test]
fn session_id_rejects_non_prefixed_wire_values() {
    assert!(serde_json::from_value::<SessionId>(json!("sess-7")).is_err());
    assert!(serde_json::from_value::<SessionId>(json!(7)).is_err());
    assert!(SessionId::from_wire("7").is_err());
}

#[test]
fn session_id_display_matches_wire_format() {
    assert_eq!(SessionId(1).to_string(), "session-1");
}

#[test]
fn turn_id_serializes_to_prefixed_string() {
    let value = serde_json::to_value(TurnId(1)).unwrap();
    assert_eq!(value, json!("turn-1"));
}
