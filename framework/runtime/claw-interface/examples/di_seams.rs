//! The two dependency-injection seams in action: `ClawFs` (persistence) and
//! `ClawHttp` (networking), each driven by a host-target reference double.
//!
//! Run with:
//!
//! ```bash
//! cargo run -p claw-interface --example di_seams \
//!     --features memfs,httpmock --target x86_64-unknown-linux-gnu
//! ```
//!
//! Core crates depend only on the `ClawFs` / `ClawHttp` *traits*; on device the
//! espidf layer implements them over FATFS and `esp_http_client`, while tests
//! and host tools substitute doubles like the `MemFs` and `ScriptedHttp` used
//! here. Both doubles live behind opt-in features and are never built into the
//! firmware.

use core::sync::atomic::AtomicBool;

use claw_interface::http::blocking::ClawHttp as _;
use claw_interface::{ClawFs, HttpAuth, HttpHeader, HttpJsonRequest, MemFs, ScriptedHttp};

fn main() -> anyhow::Result<()> {
    filesystem_seam()?;
    http_seam()?;
    Ok(())
}

/// `ClawFs`: byte-oriented persistence. The in-memory `MemFs` behaves like the
/// on-device FATFS backend for the operations the modules rely on.
fn filesystem_seam() -> anyhow::Result<()> {
    let fs = MemFs::new();

    fs.create_dir_all("/data/conversations")?;
    fs.write_atomic("/data/conversations/42.json", b"{\"version\":1}")?;
    fs.append("/data/conversations/42.jsonl", b"{\"t\":\"group\"}\n")?;

    println!("== ClawFs (MemFs) ==");
    println!("exists  -> {}", fs.exists("/data/conversations/42.json"));
    println!(
        "len     -> {} bytes",
        fs.len("/data/conversations/42.jsonl")?
    );
    println!("listing -> {:?}", fs.list_dir("/data/conversations")?);

    // A missing path is a typed error, not a panic.
    println!("missing -> {:?}", fs.read("/data/conversations/none"));
    Ok(())
}

/// `ClawHttp`: a blocking JSON POST. `ScriptedHttp` hands back canned bodies in
/// order, standing in for the `esp_http_client` driver.
fn http_seam() -> anyhow::Result<()> {
    let mut http = ScriptedHttp::new([
        r#"{"choices":[{"message":{"content":"first"}}]}"#,
        r#"{"choices":[{"message":{"content":"second"}}]}"#,
    ]);

    let abort = AtomicBool::new(false);
    let request = HttpJsonRequest {
        url: "https://api.example.com/v1/chat/completions",
        body: r#"{"model":"demo","messages":[]}"#,
        auth: HttpAuth::Bearer("sk-demo"),
        timeout_ms: 30_000,
        headers: &[HttpHeader {
            name: "X-Demo",
            value: "1",
        }],
    };

    println!("\n== ClawHttp (ScriptedHttp) ==");
    for _ in 0..2 {
        let response = http.post_json(&request, &abort)?;
        println!("status {} -> {}", response.status_code, response.body);
    }
    Ok(())
}
