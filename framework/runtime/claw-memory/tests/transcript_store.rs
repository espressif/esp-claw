use claw_interface::MemFs;
use claw_memory::TranscriptStore;

#[test]
fn discard_open_turn_drops_uncommitted_messages() {
    let store = store();
    store.push_user_message("partial");
    let before = store.version();

    store.discard_open_turn();

    assert!(store.version() > before);
    assert!(store.messages().as_array().unwrap().is_empty());
    assert!(store.open_turn_messages().is_empty());
}

#[test]
fn discard_open_turn_keeps_committed_history() {
    let store = store();
    store.push_user_message("committed");
    store.commit_open_turn();

    store.push_user_message("partial");
    store.discard_open_turn();

    let messages = store.messages();
    let messages = messages.as_array().unwrap();
    assert_eq!(messages.len(), 1);
    assert_eq!(messages[0]["content"], "committed");
}

fn store() -> TranscriptStore<MemFs> {
    MemFs::new();
    TranscriptStore::new(1, "/transcript-store-tests").unwrap()
}
