//! Integration tests for [`TranscriptStore`], exercising the public API the way
//! an agent would: append turns, read the messages and the turn snapshot,
//! persist/reload, and recover from a missing/mismatched/torn index. The store is
//! pure verbatim storage — it never compacts — so these tests assert only on the
//! full transcript being preserved across writes and reloads.

use claw_interface::{ClawFs, DiskFs, MemFs};
use claw_memory::TranscriptStore;
use serde_json::{json, Value};

// --- test doubles --------------------------------------------------------
//
// `MemFs` (hermetic in-memory) and `DiskFs` (disk-inspection) come from
// claw_interface via the `memfs` / `diskfs-pretty` dev-dependency features.

// --- helpers -------------------------------------------------------------

/// A store with default tuning, conversation id 1, and a fresh in-memory fs.
fn store_with<F: ClawFs + 'static>(_fs: F) -> TranscriptStore<F> {
    TranscriptStore::new(1, "/conversations").unwrap()
}

fn messages<F: ClawFs + 'static>(store: &TranscriptStore<F>) -> Vec<Value> {
    store
        .messages()
        .as_array()
        .cloned()
        .expect("messages() returns an array")
}

// --- tests ---------------------------------------------------------------

#[test]
fn appends_render_in_order() {
    let store = store_with(MemFs::default());

    {
        let turn = store.group();
        turn.append_user("hello");
        turn.append_assistant(r#"{"role":"assistant","content":"hi"}"#);
        turn.append_tool_result("call_1", "42", false);
    }

    let messages = messages(&store);
    assert_eq!(messages.len(), 3);
    assert_eq!(messages[0]["role"], "user");
    assert_eq!(messages[0]["content"], "hello");
    assert_eq!(messages[1]["role"], "assistant");
    assert_eq!(messages[1]["content"], "hi");
    assert_eq!(messages[2]["role"], "tool");
    assert_eq!(messages[2]["tool_call_id"], "call_1");
    assert_eq!(messages[2]["content"], "42");
    assert_eq!(messages[2]["is_error"], false);
}

#[test]
fn append_patch_expands_a_batch() {
    let store = store_with(MemFs::default());

    let batch = json!([
        { "role": "assistant", "content": "calling tool" },
        { "role": "tool", "tool_call_id": "c1", "content": "ok" },
    ]);
    {
        let turn = store.group();
        turn.append_patch(&batch);
    }

    let messages = messages(&store);
    assert_eq!(messages.len(), 2);
    assert_eq!(messages[0]["role"], "assistant");
    assert_eq!(messages[1]["tool_call_id"], "c1");
}

#[test]
fn open_turn_is_visible_then_becomes_a_committed_turn() {
    let store = store_with(MemFs::default());

    let turn = store.group();
    turn.append_user("in progress");
    // Before commit: visible in messages() and as the open turn, but not a
    // committed turn in the snapshot.
    assert_eq!(messages(&store).len(), 1);
    assert_eq!(store.open_turn_messages().len(), 1);
    assert!(store.turns_snapshot().is_empty());

    turn.commit();
    // After commit: the open turn is empty and the turn snapshot has one turn.
    assert!(store.open_turn_messages().is_empty());
    let turns = store.turns_snapshot();
    assert_eq!(turns.len(), 1);
    assert_eq!(turns[0].messages[0]["content"], "in progress");
    // Ids are 1-based (the first committed turn gets id 1).
    assert_eq!(turns[0].id.0, 1);
}

#[test]
fn direct_writer_keeps_open_turn_until_commit() {
    let store = store_with(MemFs::default());
    let patch = json!([{ "role": "assistant", "content": "working" }]);

    store.push_user_message("hello");
    store.push_patch(&patch);

    assert_eq!(messages(&store).len(), 2);
    assert_eq!(store.open_turn_messages().len(), 2);
    assert!(store.turns_snapshot().is_empty());

    store.commit_open_turn();

    assert!(store.open_turn_messages().is_empty());
    let turns = store.turns_snapshot();
    assert_eq!(turns.len(), 1);
    assert_eq!(turns[0].messages.len(), 2);
    assert_eq!(turns[0].messages[0]["content"], "hello");
    assert_eq!(turns[0].messages[1]["content"], "working");
}

#[test]
fn version_advances_on_append_and_commit() {
    let store = store_with(MemFs::default());
    let v0 = store.version();

    let turn = store.group();
    turn.append_user("x");
    let v1 = store.version();
    assert!(v1 > v0, "append must bump the version");

    turn.commit();
    let v2 = store.version();
    assert!(
        v2 > v1,
        "commit must bump the version (a new committed turn)"
    );
}

#[test]
fn distinct_ids_persist_independently() {
    MemFs::default();
    let dir = "/sessions";

    let make = |id: u32| TranscriptStore::<MemFs>::new(id, dir).unwrap();

    let one = make(1);
    let two = make(2);
    one.group().append_user("from one");
    two.group().append_user("from two");
    one.flush();
    two.flush();

    // Each id wrote its own data log (and, after flush, its index manifest).
    assert!(MemFs::exists("/sessions/conversation-1.jsonl"));
    assert!(MemFs::exists("/sessions/conversation-2.jsonl"));
    assert!(MemFs::exists("/sessions/conversation-1.json"));
    assert!(MemFs::exists("/sessions/conversation-2.json"));

    // Reloading by id restores only that conversation's transcript.
    let reloaded_one = make(1);
    assert_eq!(messages(&reloaded_one).len(), 1);
    assert_eq!(messages(&reloaded_one)[0]["content"], "from one");
}

#[test]
fn missing_persist_file_starts_empty() {
    MemFs::default();
    let store = TranscriptStore::<MemFs>::new(7, "/empty").unwrap();
    assert!(messages(&store).is_empty());
}

#[test]
fn reloads_via_manifest() {
    MemFs::default();

    let store = TranscriptStore::<MemFs>::new(1, "/c").unwrap();
    for i in 0..6 {
        store.group().append_user(format!("m{i}"));
    }
    store.flush();
    let before = messages(&store);

    // A fresh store restores the same view from the manifest + data log.
    let reloaded = TranscriptStore::<MemFs>::new(1, "/c").unwrap();
    let after = messages(&reloaded);
    assert_eq!(
        after, before,
        "reload reproduces the pre-flush view exactly"
    );
    assert_eq!(after.len(), 6);
}

#[test]
fn reloads_from_data_log_without_manifest() {
    MemFs::default();

    let store = TranscriptStore::<MemFs>::new(3, "/c").unwrap();
    store.group().append_user("a");
    store.group().append_user("b");
    store.group().append_user("c");
    store.flush();

    // Simulate a missing manifest. A fresh load must reconstruct purely from a
    // data-log scan.
    MemFs::remove("/c/conversation-3.json").ok();

    let reloaded = TranscriptStore::<MemFs>::new(3, "/c").unwrap();
    assert_eq!(messages(&reloaded).len(), 3);
    assert!(MemFs::exists("/c/conversation-3.jsonl"));
}

#[test]
fn reload_tail_scans_appends_after_manifest() {
    MemFs::default();

    let store = TranscriptStore::<MemFs>::new(4, "/c").unwrap();
    store.group().append_user("a");
    store.group().append_user("b");
    store.flush(); // manifest now covers a, b
    MemFs::append("/c/conversation-4.jsonl", &group_line(3, "c")).unwrap();

    let reloaded = TranscriptStore::<MemFs>::new(4, "/c").unwrap();
    let m = messages(&reloaded);
    assert_eq!(m.len(), 3);
    assert_eq!(m[2]["content"], "c");
}

#[test]
fn torn_trailing_line_is_ignored_on_load() {
    MemFs::default();

    // One complete record line, then a torn (newline-less) partial record.
    let mut data = serde_json::to_vec(
        &json!({ "t": "group", "id": 0, "msgs": [{ "role": "user", "content": "ok" }] }),
    )
    .unwrap();
    data.push(b'\n');
    data.extend_from_slice(br#"{"t":"group","id":1,"msgs":[{"role":"#);
    MemFs::append("/c/conversation-9.jsonl", &data).unwrap();

    let store = TranscriptStore::<MemFs>::new(9, "/c").unwrap();
    let m = messages(&store);
    assert_eq!(m.len(), 1);
    assert_eq!(m[0]["content"], "ok");
}

#[test]
fn invalid_assistant_json_is_dropped_not_panicking() {
    let store = store_with(MemFs::default());

    {
        let turn = store.group();
        turn.append_assistant("this is not json");
        turn.append_user("valid");
    }

    let messages = messages(&store);
    assert_eq!(messages.len(), 1);
    assert_eq!(messages[0]["content"], "valid");
}

fn group_line(id: u64, content: &str) -> Vec<u8> {
    let mut line = serde_json::to_vec(
        &json!({ "t": "group", "id": id, "msgs": [{ "role": "user", "content": content }] }),
    )
    .unwrap();
    line.push(b'\n');
    line
}

#[test]
fn messages_are_in_id_order() {
    let store = store_with(MemFs::default());
    for i in 0..5u32 {
        store.group().append_user(format!("turn {i}"));
    }
    let msgs = messages(&store);
    assert_eq!(msgs.len(), 5);
    for (i, msg) in msgs.iter().enumerate() {
        assert_eq!(msg["content"], format!("turn {i}"));
    }
}

#[test]
fn mismatched_manifest_triggers_rebuild_and_recovers_all_turns() {
    MemFs::default();

    // Write 3 valid groups to the data log.
    let line0 = group_line(0, "g0");
    let line1 = group_line(1, "g1");
    let line2 = group_line(2, "g2");
    let mut data = Vec::new();
    data.extend_from_slice(&line0);
    data.extend_from_slice(&line1);
    data.extend_from_slice(&line2);
    MemFs::append("/c/conversation-1.jsonl", &data).unwrap();

    // Write a manifest that claims the record at offset 0 has id=99 (wrong).
    let fake_manifest = serde_json::json!({
        "version": 1,
        "covered_len": data.len(),
        "next_id": 3,
        "live": [{ "t": "group", "id": 99, "off": 0, "len": line0.len() }]
    });
    MemFs::write_atomic(
        "/c/conversation-1.json",
        &serde_json::to_vec(&fake_manifest).unwrap(),
    )
    .unwrap();

    // Load: detects the mismatch, falls back to full scan, recovers all 3 groups,
    // and rewrites both files.
    let store = TranscriptStore::<MemFs>::new(1, "/c").unwrap();
    let msgs = messages(&store);
    assert_eq!(msgs.len(), 3, "all 3 turns recovered after rebuild");
    assert_eq!(msgs[0]["content"], "g0");
    assert_eq!(msgs[1]["content"], "g1");
    assert_eq!(msgs[2]["content"], "g2");

    // The rebuilt files should be self-consistent: reloading must give the same view.
    let reloaded = TranscriptStore::<MemFs>::new(1, "/c").unwrap();
    let reloaded_msgs = messages(&reloaded);
    assert_eq!(reloaded_msgs.len(), 3);
    assert_eq!(reloaded_msgs[0]["content"], "g0");
    assert_eq!(reloaded_msgs[2]["content"], "g2");
}

// --- disk-inspection test ------------------------------------------------

/// Drives a few turns and leaves the `.jsonl` / `.json` files in
/// `claw-memory/output/` for manual inspection.
///
/// Run with `cargo test -p claw-memory --target x86_64-unknown-linux-gnu -- \
///   --nocapture writes_inspectable_output_files` to see the file paths printed.
#[test]
#[allow(clippy::unwrap_used)]
fn writes_inspectable_output_files() {
    let output_root = concat!(env!("CARGO_MANIFEST_DIR"), "/output");
    // Virtual dir used by TranscriptStore; maps to output/data/ on disk.
    let virtual_dir = "/data";

    // Clear any files from a previous run so the output reflects exactly this test.
    let disk_dir = format!("{output_root}/data");
    std::fs::remove_dir_all(&disk_dir).ok();
    std::fs::create_dir_all(&disk_dir).expect("create disk_dir");

    DiskFs::rooted(output_root).with_pretty_json(true);
    let store = TranscriptStore::<DiskFs>::new(1, virtual_dir).unwrap();

    // Commit 6 turns; each has a user question and an assistant answer.
    for i in 0..6u32 {
        let turn = store.group();
        turn.append_user(format!("What is {i} + {i}?"));
        turn.append_assistant(&format!(
            r#"{{"role":"assistant","content":"{i} + {i} = {}"}}"#,
            i.saturating_add(i)
        ));
        // turn drops here, committing the group
    }
    store.flush();

    // The full verbatim transcript is preserved.
    let msgs = messages(&store);
    assert_eq!(msgs.len(), 12, "6 turns × (question + answer)");
    let last = msgs.last().unwrap();
    assert_eq!(last["content"], "5 + 5 = 10");

    let data_path = format!("{disk_dir}/conversation-1.jsonl");
    let index_path = format!("{disk_dir}/conversation-1.json");
    assert!(
        std::path::Path::new(&data_path).exists(),
        ".jsonl was written to disk"
    );
    assert!(
        std::path::Path::new(&index_path).exists(),
        ".json was written to disk"
    );

    // Write the rendered messages() output for inspection, pretty-printed.
    let messages_path = format!("{disk_dir}/message.json");
    let messages_json = serde_json::to_string_pretty(&*store.messages()).unwrap();
    std::fs::write(&messages_path, &messages_json).unwrap();

    println!("\n=== inspect output files ===");
    println!("data log : {data_path}");
    println!("manifest : {index_path}");
    println!("messages : {messages_path}");
    println!("{messages_json}");
}
