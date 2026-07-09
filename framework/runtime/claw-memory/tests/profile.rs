use claw_interface::{ClawFs, MemFs};
use claw_memory::{
    ProfileDocument, ProfileError, ProfileStore, DEFAULT_PROFILE_DOCUMENT_MAX_BYTES,
};

#[test]
fn missing_document_is_absent() {
    let store = store();
    assert_eq!(store.read(ProfileDocument::Soul).unwrap(), None);
}

#[test]
fn replace_and_read_round_trip() {
    let store = store();
    store.replace(ProfileDocument::Soul, "Be concise.").unwrap();
    assert_eq!(
        store.read(ProfileDocument::Soul).unwrap(),
        Some("Be concise.".to_string())
    );
}

#[test]
fn clear_keeps_file_but_returns_empty_content() {
    let store = store();
    store
        .replace(ProfileDocument::UserProfile, "Use Chinese.")
        .unwrap();
    store.clear(ProfileDocument::UserProfile).unwrap();
    assert_eq!(
        store.read(ProfileDocument::UserProfile).unwrap(),
        Some(String::new())
    );
}

#[test]
fn rejects_too_large_document() {
    let store = store();
    let content = "x".repeat(DEFAULT_PROFILE_DOCUMENT_MAX_BYTES + 1);
    let error = store
        .replace(ProfileDocument::AssistantIdentity, content)
        .unwrap_err();
    assert!(matches!(error, ProfileError::TooLarge { .. }));
}

#[test]
fn invalid_utf8_is_an_error() {
    let store = store();
    MemFs::write_atomic("/memory/soul.md", &[0xff]).unwrap();
    let error = store.read(ProfileDocument::Soul).unwrap_err();
    assert!(matches!(error, ProfileError::InvalidUtf8 { .. }));
}

#[test]
fn parses_document_ids() {
    assert_eq!("soul".parse(), Ok(ProfileDocument::Soul));
    assert_eq!("identity".parse(), Ok(ProfileDocument::AssistantIdentity));
    assert_eq!("user_profile".parse(), Ok(ProfileDocument::UserProfile));
}

fn store() -> ProfileStore<MemFs> {
    MemFs::new();
    ProfileStore::new("/memory")
}
