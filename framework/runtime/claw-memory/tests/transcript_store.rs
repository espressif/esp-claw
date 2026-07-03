//! Integration tests for [`TranscriptStore`], exercising the public API the way
//! an agent would: append turns, read the messages and the turn snapshot,
//! persist/reload, and recover from a missing/mismatched/torn index. The store is
//! pure verbatim storage — it never compacts — so these tests assert only on the
//! full transcript being preserved across writes and reloads.

use std::time::{Duration, Instant};

use claw_interface::{ClawFs, DiskFs, MemFs};
use claw_memory::{TranscriptConfig, TranscriptStore};
use serde_json::{json, Value};

// --- test doubles --------------------------------------------------------
//
// `MemFs` (hermetic in-memory) and `DiskFs` (disk-inspection) come from
// claw_interface via the `memfs` / `diskfs-pretty` dev-dependency features.

// --- helpers -------------------------------------------------------------

/// Config with an instant persist (no debounce) so appends hit disk per turn.
fn instant_config(dir: &str) -> TranscriptConfig {
    TranscriptConfig {
        persist_debounce: Duration::ZERO,
        ..TranscriptConfig::new(dir)
    }
}

/// A store with default tuning, conversation id 1, and a fresh in-memory fs.
fn store_with<F: ClawFs + 'static>(fs: F) -> TranscriptStore<F> {
    TranscriptStore::new(1, TranscriptConfig::new("/conversations"), fs).unwrap()
}

/// Poll `predicate` until it holds or a generous deadline passes (persistence
/// may settle asynchronously on a debounce; tests use instant configs but keep
/// the helper for the no-manifest crash-recovery cases).
fn wait_until(predicate: impl Fn() -> bool) -> bool {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if predicate() {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    predicate()
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
    let fs = MemFs::default();
    let dir = "/sessions";

    let make = |id: u32| TranscriptStore::new(id, TranscriptConfig::new(dir), fs.clone()).unwrap();

    let one = make(1);
    let two = make(2);
    one.group().append_user("from one");
    two.group().append_user("from two");
    one.flush();
    two.flush();

    // Each id wrote its own data log (and, after flush, its index manifest).
    assert!(fs.exists("/sessions/conversation-1.jsonl"));
    assert!(fs.exists("/sessions/conversation-2.jsonl"));
    assert!(fs.exists("/sessions/conversation-1.json"));
    assert!(fs.exists("/sessions/conversation-2.json"));

    // Reloading by id restores only that conversation's transcript.
    let reloaded_one = make(1);
    assert_eq!(messages(&reloaded_one).len(), 1);
    assert_eq!(messages(&reloaded_one)[0]["content"], "from one");
}

#[test]
fn missing_persist_file_starts_empty() {
    let store = TranscriptStore::new(7, TranscriptConfig::new("/empty"), MemFs::default()).unwrap();
    assert!(messages(&store).is_empty());
}

#[test]
fn reloads_via_manifest() {
    let fs = MemFs::default();

    let store = TranscriptStore::new(1, instant_config("/c"), fs.clone()).unwrap();
    for i in 0..6 {
        store.group().append_user(format!("m{i}"));
    }
    store.flush();
    let before = messages(&store);

    // A fresh store restores the same view from the manifest + data log.
    let reloaded = TranscriptStore::new(1, instant_config("/c"), fs.clone()).unwrap();
    let after = messages(&reloaded);
    assert_eq!(
        after, before,
        "reload reproduces the pre-flush view exactly"
    );
    assert_eq!(after.len(), 6);
}

#[test]
fn reloads_from_data_log_without_manifest() {
    let fs = MemFs::default();

    let store = TranscriptStore::new(3, instant_config("/c"), fs.clone()).unwrap();
    store.group().append_user("a");
    store.group().append_user("b");
    store.group().append_user("c");

    // Simulate the no-manifest scenario (e.g. process crash after append but
    // before the manifest is synced). A fresh load must reconstruct purely from a
    // data-log scan.
    fs.remove("/c/conversation-3.json").ok();

    let fs_for_reload = fs.clone();
    assert!(wait_until(move || {
        let reloaded =
            TranscriptStore::new(3, TranscriptConfig::new("/c"), fs_for_reload.clone()).unwrap();
        messages(&reloaded).len() == 3
    }));
    assert!(fs.exists("/c/conversation-3.jsonl"));
}

#[test]
fn reload_tail_scans_appends_after_manifest() {
    let fs = MemFs::default();

    let store = TranscriptStore::new(4, instant_config("/c"), fs.clone()).unwrap();
    store.group().append_user("a");
    store.group().append_user("b");
    store.flush(); // manifest now covers a, b
    store.group().append_user("c"); // appended past covered_len; manifest left stale

    let fs_for_reload = fs.clone();
    assert!(wait_until(move || {
        let reloaded =
            TranscriptStore::new(4, TranscriptConfig::new("/c"), fs_for_reload.clone()).unwrap();
        let m = messages(&reloaded);
        m.len() == 3 && m[2]["content"] == "c"
    }));
}

#[test]
fn torn_trailing_line_is_ignored_on_load() {
    let fs = MemFs::default();

    // One complete record line, then a torn (newline-less) partial record.
    let mut data = serde_json::to_vec(
        &json!({ "t": "group", "id": 0, "msgs": [{ "role": "user", "content": "ok" }] }),
    )
    .unwrap();
    data.push(b'\n');
    data.extend_from_slice(br#"{"t":"group","id":1,"msgs":[{"role":"#);
    fs.append("/c/conversation-9.jsonl", &data).unwrap();

    let store = TranscriptStore::new(9, TranscriptConfig::new("/c"), fs.clone()).unwrap();
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
    let fs = MemFs::default();

    // Write 3 valid groups to the data log.
    let line0 = group_line(0, "g0");
    let line1 = group_line(1, "g1");
    let line2 = group_line(2, "g2");
    let mut data = Vec::new();
    data.extend_from_slice(&line0);
    data.extend_from_slice(&line1);
    data.extend_from_slice(&line2);
    fs.append("/c/conversation-1.jsonl", &data).unwrap();

    // Write a manifest that claims the record at offset 0 has id=99 (wrong).
    let fake_manifest = serde_json::json!({
        "version": 1,
        "covered_len": data.len(),
        "next_id": 3,
        "live": [{ "t": "group", "id": 99, "off": 0, "len": line0.len() }]
    });
    fs.write_atomic(
        "/c/conversation-1.json",
        &serde_json::to_vec(&fake_manifest).unwrap(),
    )
    .unwrap();

    // Load: detects the mismatch, falls back to full scan, recovers all 3 groups,
    // and rewrites both files.
    let store = TranscriptStore::new(1, TranscriptConfig::new("/c"), fs.clone()).unwrap();
    let msgs = messages(&store);
    assert_eq!(msgs.len(), 3, "all 3 turns recovered after rebuild");
    assert_eq!(msgs[0]["content"], "g0");
    assert_eq!(msgs[1]["content"], "g1");
    assert_eq!(msgs[2]["content"], "g2");

    // The rebuilt files should be self-consistent: reloading must give the same view.
    let reloaded = TranscriptStore::new(1, TranscriptConfig::new("/c"), fs.clone()).unwrap();
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
    // Virtual dir used by TranscriptConfig; maps to output/data/ on disk.
    let virtual_dir = "/data";

    // Clear any files from a previous run so the output reflects exactly this test.
    let disk_dir = format!("{output_root}/data");
    std::fs::remove_dir_all(&disk_dir).ok();
    std::fs::create_dir_all(&disk_dir).expect("create disk_dir");

    let fs = DiskFs::rooted(output_root).with_pretty_json(true);
    let store = TranscriptStore::new(1, instant_config(virtual_dir), fs.clone()).unwrap();

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
