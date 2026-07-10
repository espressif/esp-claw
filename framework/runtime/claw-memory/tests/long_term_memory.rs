#![allow(clippy::unwrap_used)]

use claw_interface::{ClawFs, MemFs};
use claw_memory::{
    LongTermError, LongTermMemory, MemoryDraft, MemoryId, MemoryPatch, StoreOutcome,
};

#[test]
fn store_mints_prefixed_ids_in_order() {
    reset_fs();
    let memory = memory();
    let first = memory.store(draft("Likes tea", &["preference"]));
    let second = memory.store(draft("Lives in Berlin", &["fact"]));
    assert_eq!(first.item().id.as_str(), "g-0");
    assert_eq!(second.item().id.as_str(), "g-1");
}

#[test]
fn store_dedups_by_normalized_content() {
    reset_fs();
    let memory = memory();
    assert!(matches!(
        memory.store(draft("Likes  TEA", &["preference"])),
        StoreOutcome::Created(_)
    ));
    match memory.store(draft("likes tea", &["preference"])) {
        StoreOutcome::Duplicate(item) => assert_eq!(item.id.as_str(), "g-0"),
        other => panic!("expected duplicate, got {other:?}"),
    }
    assert_eq!(memory.list().len(), 1);
}

#[test]
fn recall_filters_by_label_and_query_newest_first() {
    reset_fs();
    let memory = memory();
    memory.store(draft("Likes tea", &["preference"]));
    memory.store(draft("Likes coffee too", &["preference"]));
    memory.store(draft("Has a dog", &["fact"]));

    let prefs = memory.recall(&["preference".to_string()], None, 10);
    assert_eq!(prefs.len(), 2);
    assert_eq!(prefs[0].content, "Likes coffee too");

    let tea = memory.recall(&["preference".to_string()], Some("tea"), 10);
    assert_eq!(tea.len(), 1);
    assert_eq!(tea[0].content, "Likes tea");

    assert_eq!(memory.recall(&[], None, 2).len(), 2);
}

#[test]
fn update_replaces_only_supplied_fields() {
    reset_fs();
    let memory = memory();
    let id = memory.store(draft("Old", &["x"])).item().id.clone();
    let updated = memory
        .update(
            &id,
            MemoryPatch {
                content: Some("New".to_string()),
                ..Default::default()
            },
        )
        .expect("update");
    assert_eq!(updated.content, "New");
    assert_eq!(updated.tags, vec!["x".to_string()]);
    assert!(matches!(
        memory.update(&MemoryId::from("g-999"), MemoryPatch::default()),
        Err(LongTermError::NotFound(_))
    ));
}

#[test]
fn forget_removes_the_item() {
    reset_fs();
    let memory = memory();
    let id = memory.store(draft("Ephemeral", &["x"])).item().id.clone();
    memory.forget(&id).expect("forget");
    assert!(memory.list().is_empty());
    assert!(matches!(
        memory.forget(&id),
        Err(LongTermError::NotFound(_))
    ));
}

#[test]
fn state_survives_reload_from_journal() {
    reset_fs();
    {
        let memory = LongTermMemory::<MemFs>::new("/m", "g-").expect("load empty store");
        memory.store(draft("Persistent", &["fact"]));
        let id = memory
            .store(draft("To be edited", &["fact"]))
            .item()
            .id
            .clone();
        memory
            .update(
                &id,
                MemoryPatch {
                    content: Some("Edited".to_string()),
                    ..Default::default()
                },
            )
            .expect("update");
    }
    let reloaded = LongTermMemory::<MemFs>::new("/m", "g-").expect("replay journal");
    assert_eq!(reloaded.list().len(), 2);
    let edited = reloaded.recall(&[], Some("edited"), 10);
    assert_eq!(edited.len(), 1);
    assert_eq!(edited[0].content, "Edited");
    assert_eq!(
        reloaded
            .store(draft("Another", &["fact"]))
            .item()
            .id
            .as_str(),
        "g-2"
    );
}

#[test]
fn torn_trailing_journal_record_is_ignored_on_reload() {
    reset_fs();
    {
        let memory = LongTermMemory::<MemFs>::new("/m", "g-").expect("load empty store");
        memory.store(draft("Committed before crash", &["fact"]));
    }
    MemFs::append("/m/memory_records.jsonl", br#"{"torn":"record""#).unwrap();

    let reloaded = LongTermMemory::<MemFs>::new("/m", "g-").expect("replay journal");
    let items = reloaded.list();
    assert_eq!(items.len(), 1);
    assert_eq!(items[0].content, "Committed before crash");
    assert_eq!(
        reloaded
            .store(draft("After crash", &["fact"]))
            .item()
            .id
            .as_str(),
        "g-1"
    );
}

fn memory() -> LongTermMemory<MemFs> {
    LongTermMemory::new("/m", "g-").expect("load empty store")
}

fn reset_fs() {
    MemFs::new();
}

fn draft(content: &str, tags: &[&str]) -> MemoryDraft {
    MemoryDraft::new(content).with_tags(tags.iter().map(|tag| (*tag).to_string()))
}
