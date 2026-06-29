//! Integration tests for [`ConversationMemory`], exercising the public API the
//! way an agent would: append turns, read the messages, persist/reload, and let
//! background auto-compaction run on the shared pool.

use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use claw_interface::{ClawFs, DiskFs, MemFs, StdThread};
use claw_memory::{
    CompactError, Compactor, ConversationConfig, ConversationDeps, ConversationMemory,
};
use claw_utils::{PoolConfig, SharedTaskPool};
use serde_json::{json, Value};

// --- test doubles --------------------------------------------------------
//
// `MemFs` (hermetic in-memory) and `DiskFs` (disk-inspection) come from
// claw_interface via the `memfs` / `diskfs-pretty` dev-dependency features.

/// A [`Compactor`] that records how many times it ran and returns a fixed
/// one-message summary, so tests can observe the background compaction.
struct StubCompactor {
    calls: AtomicUsize,
    marker: String,
}

impl StubCompactor {
    fn new(marker: &str) -> Self {
        Self {
            calls: AtomicUsize::new(0),
            marker: marker.to_string(),
        }
    }
}

impl Compactor for StubCompactor {
    fn compact(&self, _window: &[Value]) -> Result<Vec<Value>, CompactError> {
        self.calls.fetch_add(1, Ordering::SeqCst);
        Ok(vec![json!({ "role": "system", "content": self.marker })])
    }
}

// --- helpers -------------------------------------------------------------

fn pool() -> Arc<SharedTaskPool> {
    Arc::new(SharedTaskPool::new(PoolConfig::default(), StdThread).expect("spawn pool"))
}

fn deps<F: ClawFs + 'static>(
    fs: F,
    pool: Arc<SharedTaskPool>,
    marker: &str,
) -> ConversationDeps<F> {
    ConversationDeps {
        fs,
        pool,
        compactor: Arc::new(StubCompactor::new(marker)),
    }
}

/// Config with an instant persist (no debounce) so appends hit disk per turn.
///
/// `keep_recent_tokens` sets the verbatim-tail token budget. `segment_token_budget`
/// is set to `usize::MAX` so all aged groups are compacted in a single chunk,
/// preserving deterministic single-cycle compaction in tests.
fn instant_config(dir: &str, threshold: usize, keep_recent_tokens: usize) -> ConversationConfig {
    let mut config = ConversationConfig::new(dir);
    config.compact_threshold_tokens = threshold;
    config.keep_recent_tokens = keep_recent_tokens;
    config.segment_token_budget = usize::MAX;
    config.persist_debounce = Duration::ZERO;
    config
}

/// A memory with default tuning, conversation id 1, and fresh doubles.
fn memory_with<F: ClawFs + 'static>(fs: F) -> ConversationMemory<F> {
    ConversationMemory::new(
        1,
        ConversationConfig::new("/conversations"),
        ConversationDeps {
            fs,
            pool: pool(),
            compactor: Arc::new(StubCompactor::new("S")),
        },
    )
}

/// Poll `predicate` until it holds or a generous deadline passes (background
/// jobs run on the pool, so observable state settles asynchronously).
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

fn messages<F: ClawFs + 'static>(memory: &ConversationMemory<F>) -> Vec<Value> {
    memory
        .messages()
        .as_array()
        .cloned()
        .expect("messages() returns an array")
}

/// True when `messages` is the stable compacted shape: one or more leading
/// `SUMMARY` system segments followed by exactly `verbatim_tail` verbatim
/// messages in order.
///
/// Compaction runs one chunk per background job, so the number of summary
/// segments is timing-dependent (a single job may cover all aged groups, or
/// several jobs may each cover a slice). Tests therefore assert on the tail and
/// on "everything before it is a summary", not on an exact message count.
fn is_compacted_with_tail(
    messages: &[Value],
    summary_marker: &str,
    verbatim_tail: &[&str],
) -> bool {
    if messages.len() < verbatim_tail.len() + 1 {
        return false; // need at least one summary segment plus the tail
    }
    let split = messages.len() - verbatim_tail.len();
    let (summaries, tail) = messages.split_at(split);
    summaries.iter().all(|m| m["content"] == summary_marker)
        && tail
            .iter()
            .zip(verbatim_tail)
            .all(|(message, expected)| message["content"] == *expected)
}

/// Drive turn boundaries until `predicate` holds or a deadline passes.
///
/// Compaction is computed in the background and only *applied* on the next turn
/// boundary, so a real agent reaches the compacted state by continuing to commit
/// turns (or flushing). An empty `group()` commit is exactly such a boundary: it
/// applies any finished summary and reschedules the next one if still over
/// budget. This mirrors that loop deterministically in tests.
fn pump_until<F: ClawFs + 'static>(
    memory: &mut ConversationMemory<F>,
    predicate: impl Fn(&ConversationMemory<F>) -> bool,
) -> bool {
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        drop(memory.group()); // apply a finished summary / reschedule
        if predicate(memory) {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
    }
    predicate(memory)
}

// --- tests ---------------------------------------------------------------

#[test]
fn appends_render_in_order() {
    let mut memory = memory_with(MemFs::default());

    {
        let turn = memory.group();
        turn.append_user("hello");
        turn.append_assistant(r#"{"role":"assistant","content":"hi"}"#);
        turn.append_tool_result("call_1", "42", false);
    }

    let messages = messages(&memory);
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
    let mut memory = memory_with(MemFs::default());

    let batch = json!([
        { "role": "assistant", "content": "calling tool" },
        { "role": "tool", "tool_call_id": "c1", "content": "ok" },
    ]);
    {
        let turn = memory.group();
        turn.append_patch(&batch);
    }

    let messages = messages(&memory);
    assert_eq!(messages.len(), 2);
    assert_eq!(messages[0]["role"], "assistant");
    assert_eq!(messages[1]["tool_call_id"], "c1");
}

#[test]
fn distinct_ids_persist_independently() {
    let fs = MemFs::default();
    let shared_pool = pool();
    let dir = "/sessions";

    let make = |id: usize| {
        ConversationMemory::new(
            id,
            ConversationConfig::new(dir),
            ConversationDeps {
                fs: fs.clone(),
                pool: Arc::clone(&shared_pool),
                compactor: Arc::new(StubCompactor::new("S")),
            },
        )
    };

    let mut one = make(1);
    let mut two = make(2);
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
    let memory = ConversationMemory::new(
        7,
        ConversationConfig::new("/empty"),
        ConversationDeps {
            fs: MemFs::default(),
            pool: pool(),
            compactor: Arc::new(StubCompactor::new("S")),
        },
    );
    assert!(messages(&memory).is_empty());
}

#[test]
fn crosses_threshold_and_auto_compacts_in_background() {
    let compactor = Arc::new(StubCompactor::new("SUMMARY"));

    let mut config = ConversationConfig::new("/conversations");
    config.compact_threshold_tokens = 5; // tiny, so a few turns trip it
                                         // keep_recent_tokens: "message number N" serialises to ~43 chars (~11 tokens).
                                         // 15 tokens keeps 2 groups: group-5 gives 11 < 15 so we continue; group-4
                                         // brings the total to 22 >= 15, stopping at count=2.
    config.keep_recent_tokens = 15;
    // compact all aged groups in one shot so the test converges in one cycle.
    config.segment_token_budget = usize::MAX;

    let mut memory = ConversationMemory::new(
        1,
        config,
        ConversationDeps {
            fs: MemFs::default(),
            pool: pool(),
            compactor: Arc::clone(&compactor) as Arc<dyn Compactor>,
        },
    );

    // Each message is its own turn, so they age out as distinct groups.
    for i in 0..6 {
        memory.group().append_user(format!("message number {i}"));
    }

    // The summary is computed on a pool worker and applied at a later turn
    // boundary, so drive boundaries until it settles: one or more summary
    // segments followed by the `keep_recent_tokens` newest verbatim turns.
    assert!(
        pump_until(&mut memory, |m| {
            is_compacted_with_tail(
                &messages(m),
                "SUMMARY",
                &["message number 4", "message number 5"],
            )
        }),
        "expected summary segments + 2 recent messages, got {:?}",
        messages(&memory),
    );

    // The Compactor ran on the pool, off the append path.
    assert!(compactor.calls.load(Ordering::SeqCst) >= 1);
}

#[test]
fn reloads_after_compaction_via_manifest() {
    let fs = MemFs::default();
    let shared = pool();

    // "mN" serialises to ~29 chars (~8 tokens/group). keep_recent_tokens=9 keeps
    // 2 verbatim groups: group-5 gives 8 < 9 so we continue; group-4 gives 16 >= 9.
    let mut memory = ConversationMemory::new(
        1,
        instant_config("/c", 5, 9),
        deps(fs.clone(), Arc::clone(&shared), "SUMMARY"),
    );
    for i in 0..6 {
        memory.group().append_user(format!("m{i}"));
    }
    assert!(pump_until(&mut memory, |m| {
        is_compacted_with_tail(&messages(m), "SUMMARY", &["m4", "m5"])
    }));
    memory.flush();
    let before = messages(&memory);

    // A fresh memory restores the same view from the manifest + data log.
    let reloaded = ConversationMemory::new(
        1,
        instant_config("/c", 5, 9),
        deps(fs.clone(), Arc::clone(&shared), "SUMMARY"),
    );
    let after = messages(&reloaded);
    assert_eq!(
        after, before,
        "reload reproduces the pre-flush view exactly"
    );
    assert!(is_compacted_with_tail(&after, "SUMMARY", &["m4", "m5"]));
}

#[test]
fn reloads_from_data_log_without_manifest() {
    let fs = MemFs::default();
    let shared = pool();

    let mut memory = ConversationMemory::new(
        3,
        instant_config("/c", 100_000, 999_999),
        deps(fs.clone(), Arc::clone(&shared), "S"),
    );
    memory.group().append_user("a");
    memory.group().append_user("b");
    memory.group().append_user("c");

    // Simulate the no-manifest scenario (e.g. process crash after append but before
    // the manifest is synced). A fresh load must reconstruct purely from a data-log scan.
    fs.remove("/c/conversation-3.json").ok();

    let fs_for_reload = fs.clone();
    let pool_for_reload = Arc::clone(&shared);
    assert!(wait_until(move || {
        let reloaded = ConversationMemory::new(
            3,
            ConversationConfig::new("/c"),
            deps(fs_for_reload.clone(), Arc::clone(&pool_for_reload), "S"),
        );
        messages(&reloaded).len() == 3
    }));
    assert!(fs.exists("/c/conversation-3.jsonl"));
}

#[test]
fn reload_tail_scans_appends_after_manifest() {
    let fs = MemFs::default();
    let shared = pool();

    let mut memory = ConversationMemory::new(
        4,
        instant_config("/c", 100_000, 999_999),
        deps(fs.clone(), Arc::clone(&shared), "S"),
    );
    memory.group().append_user("a");
    memory.group().append_user("b");
    memory.flush(); // manifest now covers a, b
    memory.group().append_user("c"); // appended past covered_len; manifest left stale

    let fs_for_reload = fs.clone();
    let pool_for_reload = Arc::clone(&shared);
    assert!(wait_until(move || {
        let reloaded = ConversationMemory::new(
            4,
            ConversationConfig::new("/c"),
            deps(fs_for_reload.clone(), Arc::clone(&pool_for_reload), "S"),
        );
        let m = messages(&reloaded);
        m.len() == 3 && m[2]["content"] == "c"
    }));
}

#[test]
fn collapse_reclaims_dead_bytes() {
    let memfs = MemFs::default();
    let fs = memfs.clone();

    let mut memory = ConversationMemory::new(
        5,
        instant_config("/c", 5, 9),
        deps(fs.clone(), pool(), "SUMMARY"),
    );
    let big = "x".repeat(1024);
    for i in 0..60 {
        memory.group().append_user(format!("{big}{i}"));
    }

    // Append-only growth would push the data log well past 60 KiB; collapse must
    // rewrite it from the small live set, keeping it bounded. Pump turn boundaries
    // so the background summaries get applied (and dead bytes reclaimed).
    assert!(pump_until(&mut memory, |_| {
        memfs
            .len("/c/conversation-5.jsonl")
            .is_ok_and(|n| n < 30 * 1024)
    }));
    let m = messages(&memory);
    assert_eq!(m[0]["content"], "SUMMARY");
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

    let memory = ConversationMemory::new(
        9,
        ConversationConfig::new("/c"),
        deps(fs.clone(), pool(), "S"),
    );
    let m = messages(&memory);
    assert_eq!(m.len(), 1);
    assert_eq!(m[0]["content"], "ok");
}

#[test]
fn invalid_assistant_json_is_dropped_not_panicking() {
    let mut memory = memory_with(MemFs::default());

    {
        let turn = memory.group();
        turn.append_assistant("this is not json");
        turn.append_user("valid");
    }

    let messages = messages(&memory);
    assert_eq!(messages.len(), 1);
    assert_eq!(messages[0]["content"], "valid");
}

// --- message ordering tests ----------------------------------------------

fn group_line(id: u64, content: &str) -> Vec<u8> {
    let mut line = serde_json::to_vec(
        &json!({ "t": "group", "id": id, "msgs": [{ "role": "user", "content": content }] }),
    )
    .unwrap();
    line.push(b'\n');
    line
}

fn compact_line(id_start: u64, id_end: u64, summary_content: &str) -> Vec<u8> {
    let mut line = serde_json::to_vec(&json!({
        "t": "compact",
        "id_start": id_start,
        "id_end": id_end,
        "summary": [{ "role": "system", "content": summary_content }],
    }))
    .unwrap();
    line.push(b'\n');
    line
}

fn load_from_log(data: Vec<u8>) -> ConversationMemory<MemFs> {
    let fs = MemFs::default();
    fs.append("/c/conversation-1.jsonl", &data).unwrap();
    ConversationMemory::new(1, ConversationConfig::new("/c"), deps(fs, pool(), "S"))
}

#[test]
fn mismatched_manifest_triggers_rebuild_and_recovers_all_groups() {
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
    let memory = ConversationMemory::new(
        1,
        ConversationConfig::new("/c"),
        deps(fs.clone(), pool(), "S"),
    );
    let msgs = messages(&memory);
    assert_eq!(msgs.len(), 3, "all 3 groups recovered after rebuild");
    assert_eq!(msgs[0]["content"], "g0");
    assert_eq!(msgs[1]["content"], "g1");
    assert_eq!(msgs[2]["content"], "g2");

    // The rebuilt files should be self-consistent: reloading must give the same view.
    let reloaded = ConversationMemory::new(
        1,
        ConversationConfig::new("/c"),
        deps(fs.clone(), pool(), "S"),
    );
    let reloaded_msgs = messages(&reloaded);
    assert_eq!(reloaded_msgs.len(), 3);
    assert_eq!(reloaded_msgs[0]["content"], "g0");
    assert_eq!(reloaded_msgs[2]["content"], "g2");
}

#[test]
fn messages_without_compaction_are_in_id_order() {
    let mut memory = memory_with(MemFs::default());
    for i in 0..5u32 {
        memory.group().append_user(format!("turn {i}"));
    }
    let msgs = messages(&memory);
    assert_eq!(msgs.len(), 5);
    for (i, msg) in msgs.iter().enumerate() {
        assert_eq!(msg["content"], format!("turn {i}"));
    }
}

#[test]
fn messages_with_leading_compaction_places_summary_first() {
    // groups 0-4 compacted, groups 5-9 verbatim → summary first, then groups 5-9
    let mut data = Vec::new();
    for i in 0u64..5 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }
    data.extend_from_slice(&compact_line(0, 4, "SUMMARY"));
    for i in 5u64..10 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }

    let memory = load_from_log(data);
    let msgs = messages(&memory);
    assert_eq!(msgs.len(), 6, "summary + 5 verbatim groups");
    assert_eq!(msgs[0]["content"], "SUMMARY");
    for i in 0..5usize {
        assert_eq!(msgs[1 + i]["content"], format!("g{}", i + 5));
    }
}

#[test]
fn messages_with_mid_range_compaction_inserts_summary_at_correct_position() {
    // groups 0-4 verbatim, groups 5-9 compacted, groups 10-14 verbatim
    // expected: g0..g4, SUMMARY, g10..g14
    let mut data = Vec::new();
    for i in 0u64..5 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }
    data.extend_from_slice(&compact_line(5, 9, "SUMMARY"));
    for i in 10u64..15 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }

    let memory = load_from_log(data);
    let msgs = messages(&memory);
    assert_eq!(msgs.len(), 11, "5 before + summary + 5 after");
    for i in 0..5usize {
        assert_eq!(msgs[i]["content"], format!("g{i}"), "position {i}");
    }
    assert_eq!(msgs[5]["content"], "SUMMARY");
    for i in 0..5usize {
        assert_eq!(
            msgs[6 + i]["content"],
            format!("g{}", i + 10),
            "position {}",
            6 + i
        );
    }
}

#[test]
fn messages_ids_1_to_20_with_4_to_10_compacted() {
    // Simulates the post-compaction data log: groups 1-3 are verbatim, groups
    // 4-10 have been replaced by a compact record, groups 11-20 are verbatim.
    // Expected order: g1, g2, g3, SUMMARY, g11 .. g20 (14 messages total).
    let mut data = Vec::new();
    for i in 1u64..=3 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }
    data.extend_from_slice(&compact_line(4, 10, "SUMMARY"));
    for i in 11u64..=20 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }

    let memory = load_from_log(data);
    let msgs = messages(&memory);
    assert_eq!(msgs.len(), 14, "3 uncompacted + summary + 10 uncompacted");

    assert_eq!(msgs[0]["content"], "g1");
    assert_eq!(msgs[1]["content"], "g2");
    assert_eq!(msgs[2]["content"], "g3");

    assert_eq!(msgs[3]["content"], "SUMMARY");

    for i in 0..10usize {
        assert_eq!(
            msgs[4 + i]["content"],
            format!("g{}", i + 11),
            "position {}",
            4 + i
        );
    }
}

#[test]
fn compact_record_supersedes_groups_in_range() {
    // All 20 groups (ids 1-20) are written first, then a compact record is
    // appended covering 4-10. The load path must retire groups 4-10 and treat
    // the compact record as their replacement, leaving 1-3 and 11-20 verbatim.
    let mut data = Vec::new();
    for i in 1u64..=20 {
        data.extend_from_slice(&group_line(i, &format!("g{i}")));
    }
    data.extend_from_slice(&compact_line(4, 10, "SUMMARY"));

    let memory = load_from_log(data);
    let msgs = messages(&memory);
    assert_eq!(msgs.len(), 14, "3 uncompacted + summary + 10 uncompacted");

    assert_eq!(msgs[0]["content"], "g1");
    assert_eq!(msgs[1]["content"], "g2");
    assert_eq!(msgs[2]["content"], "g3");

    assert_eq!(msgs[3]["content"], "SUMMARY");

    for i in 0..10usize {
        assert_eq!(
            msgs[4 + i]["content"],
            format!("g{}", i + 11),
            "position {}",
            4 + i
        );
    }
}

// --- manifest coverage helpers -------------------------------------------

/// Number of live compact segments listed in a manifest `.json`.
fn count_compacts(index_json: &str) -> usize {
    serde_json::from_str::<Value>(index_json)
        .ok()
        .and_then(|m| {
            m["live"]
                .as_array()
                .map(|live| live.iter().filter(|e| e["t"] == "compact").count())
        })
        .unwrap_or(0)
}

/// Assert the manifest's live records cover a contiguous run of ids with no
/// gaps and no overlaps, returning `(min_id, max_id)`.
///
/// This is the index the user inspects: compact segments (`[id_start, id_end]`)
/// plus single-id groups must tile the whole committed id range without holes.
#[allow(clippy::unwrap_used, clippy::indexing_slicing)]
fn assert_manifest_coverage_contiguous(index_json: &str) -> (u64, u64) {
    let manifest: Value = serde_json::from_str(index_json).expect("parse manifest");
    let live = manifest["live"].as_array().expect("live array");

    let mut spans: Vec<(u64, u64)> = live
        .iter()
        .map(|e| match e["t"].as_str() {
            Some("compact") => (
                e["id_start"].as_u64().expect("id_start"),
                e["id_end"].as_u64().expect("id_end"),
            ),
            Some("group") => {
                let id = e["id"].as_u64().expect("id");
                (id, id)
            }
            other => panic!("unexpected manifest entry type: {other:?}"),
        })
        .collect();
    spans.sort_by_key(|(start, _)| *start);
    assert!(!spans.is_empty(), "manifest has no live records");

    for window in spans.windows(2) {
        let (_, prev_end) = window[0];
        let (start, _) = window[1];
        assert_eq!(
            start,
            prev_end + 1,
            "hole/overlap in coverage: a span ending at {prev_end} is followed by {start} \
             (expected {})",
            prev_end + 1
        );
    }
    (spans[0].0, spans.last().unwrap().1)
}

/// Commits many turns under a small `segment_token_budget` so several compact
/// segments accumulate, then asserts the on-disk index covers every committed
/// id with no holes — the regression test for the drop-oldest data-loss bug.
#[test]
#[allow(clippy::unwrap_used)]
fn compaction_index_coverage_has_no_holes() {
    let output_root = concat!(env!("CARGO_MANIFEST_DIR"), "/output");
    let virtual_dir = "/holes";
    let disk_dir = format!("{output_root}/holes");
    std::fs::remove_dir_all(&disk_dir).ok();
    std::fs::create_dir_all(&disk_dir).expect("create disk_dir");

    let fs = DiskFs::rooted(output_root).with_pretty_json(true);
    let shared = pool();

    let mut cfg = ConversationConfig::new(virtual_dir);
    cfg.compact_threshold_tokens = 20; // trip after a few turns
    cfg.keep_recent_tokens = 10; // keep ~1 newest turn verbatim
    cfg.segment_token_budget = 12; // small → each chunk is one group → many segments
    cfg.persist_debounce = Duration::ZERO;

    let mut memory = ConversationMemory::new(2, cfg, deps(fs.clone(), Arc::clone(&shared), "S"));

    for i in 0..15u32 {
        memory.group().append_user(format!("turn {i}"));
    }

    let index_path = format!("{disk_dir}/conversation-2.json");
    // Drive turn boundaries until at least two compact segments exist on disk.
    let index_for_poll = index_path.clone();
    assert!(
        pump_until(&mut memory, |_| {
            std::fs::read_to_string(&index_for_poll)
                .map(|s| count_compacts(&s) >= 2)
                .unwrap_or(false)
        }),
        "expected multiple compact segments to form"
    );
    memory.flush();

    let index_json = std::fs::read_to_string(&index_path).unwrap();
    let (min_id, max_id) = assert_manifest_coverage_contiguous(&index_json);
    assert_eq!(min_id, 1, "coverage starts at the first committed id");
    assert!(max_id >= 1, "coverage reaches the newest committed id");
}

// --- disk-inspection test ------------------------------------------------

/// Runs a realistic compaction scenario and leaves the `.jsonl` / `.json` files
/// in `claw-memory/output/` for manual inspection.
///
/// Run with `cargo test -p claw-memory --target x86_64-unknown-linux-gnu -- \
///   --nocapture writes_inspectable_output_files` to see the file paths printed.
#[test]
#[allow(clippy::unwrap_used)]
fn writes_inspectable_output_files() {
    let output_root = concat!(env!("CARGO_MANIFEST_DIR"), "/output");
    // Virtual dir used by ConversationConfig; maps to output/data/ on disk.
    let virtual_dir = "/data";

    // Clear any files from a previous run so the output reflects exactly this test.
    let disk_dir = format!("{output_root}/data");
    std::fs::remove_dir_all(&disk_dir).ok();
    std::fs::create_dir_all(&disk_dir).expect("create disk_dir");

    let fs = DiskFs::rooted(output_root).with_pretty_json(true);
    let shared = pool();

    let mut cfg = ConversationConfig::new(virtual_dir);
    // Low threshold so compaction triggers after a few turns.
    cfg.compact_threshold_tokens = 5;
    // "What is N + N?" serialises to ~50 chars (~13 tokens/group); keeping
    // keep_recent_tokens=20 retains the 2 newest turns verbatim (13 < 20,
    // continue; 13+13=26 >= 20, stop at count=2).
    cfg.keep_recent_tokens = 20;
    // Compact all aged groups in a single chunk so the output is easy to read.
    cfg.segment_token_budget = usize::MAX;
    cfg.persist_debounce = Duration::ZERO;

    let mut memory = ConversationMemory::new(
        1,
        cfg,
        deps(fs.clone(), Arc::clone(&shared), "[COMPACTED SUMMARY]"),
    );

    // Commit 6 turns; each has a user question and an assistant answer.
    for i in 0..6u32 {
        let turn = memory.group();
        turn.append_user(format!("What is {i} + {i}?"));
        turn.append_assistant(&format!(
            r#"{{"role":"assistant","content":"{i} + {i} = {}"}}"#,
            i.saturating_add(i)
        ));
        // turn drops here, committing the group
    }

    // Wait for the background compaction and apply it at a turn boundary.
    assert!(
        pump_until(&mut memory, |m| messages(m)
            .iter()
            .any(|msg| msg["content"] == "[COMPACTED SUMMARY]")),
        "expected at least one compact segment to appear"
    );

    // Force all pending writes and the updated manifest to disk.
    memory.flush();

    // Verify the logical view.
    let msgs = messages(&memory);
    assert!(
        msgs.iter()
            .any(|msg| msg["content"] == "[COMPACTED SUMMARY]"),
        "compact segment present in messages()"
    );
    // The newest turn is kept verbatim, so the last message is its assistant reply.
    let last_verbatim = msgs.last().unwrap();
    assert_eq!(
        last_verbatim["content"], "5 + 5 = 10",
        "last verbatim message is the final assistant reply"
    );

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

    // The index must tile the whole id range with no holes.
    let index_json = std::fs::read_to_string(&index_path).unwrap();
    let (min_id, _) = assert_manifest_coverage_contiguous(&index_json);
    assert_eq!(min_id, 1, "coverage starts at the first committed id");

    // Write the rendered messages() output for inspection, pretty-printed.
    let messages_path = format!("{disk_dir}/message.json");
    let messages_json = serde_json::to_string_pretty(&*memory.messages()).unwrap();
    std::fs::write(&messages_path, &messages_json).unwrap();

    println!("\n=== inspect output files ===");
    println!("data log : {data_path}");
    println!("manifest : {index_path}");
    println!("messages : {messages_path}");
    println!("{messages_json}");
}
