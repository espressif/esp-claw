//! Walk through what the agent can and cannot touch from inside a sandbox.
//!
//! Run: `cargo run --example sandbox_demo -p claw-sandbox`
//!
//! A single in-memory [`MemFs`] plays the role of the real backing store. The
//! sandbox maps each visible virtual root onto a real path inside it:
//! - `/sandbox/*`        → the per-instance host dir (private, ephemeral)
//! - `/shared/{skills,tmp,data}/*` → fixed shared locations (shared with host)
//! - `/system/skills/*`  → a read-only system location
//!
//! Everything else — bare roots, unlisted paths, `..` escapes — is rejected.

use claw_interface::{ClawFs, MemFs};
use claw_sandbox::{RealRoots, Sandbox, SandboxError, SandboxFs};

fn main() -> anyhow::Result<()> {
    // The real backing store. `MemFs` stores data behind its static HAL, so we
    // can peek at the raw real paths and see where the sandbox routed writes.
    MemFs::new();

    let sandbox = Sandbox::<MemFs>::new(
        "/data/sandboxes/inst-1",
        RealRoots {
            shared_skills: "/data/shared/skills",
            shared_tmp: "/data/shared/tmp",
            shared_data: "/data/shared/data",
            system_skills: "/system/skills",
        },
    )?;

    // --- The private, ephemeral root --------------------------------------
    // Scratch work the agent does for this run only.
    sandbox.write_atomic("/sandbox/tmp/scratch.txt", b"working...")?;
    let scratch = sandbox.read("/sandbox/tmp/scratch.txt")?;
    println!(
        "/sandbox/tmp/scratch.txt -> {:?}",
        String::from_utf8_lossy(&scratch)
    );

    // It really lands under the per-instance host dir in the backing store:
    let raw = MemFs::read("/data/sandboxes/inst-1/tmp/scratch.txt")?;
    println!("  (backing real path holds {} bytes)", raw.len());

    // The scratch roots were materialized at construction, so they list empty
    // instead of erroring.
    println!(
        "/sandbox/skills listing -> {:?}",
        sandbox.list_dir("/sandbox/skills")?
    );

    // --- The shared, persistent root --------------------------------------
    // A result the agent wants to hand back across the sandbox boundary.
    sandbox.write_atomic("/shared/data/report.md", b"# Result\n")?;
    // The host, outside the sandbox, reads it at the shared real path:
    let from_host = MemFs::read("/data/shared/data/report.md")?;
    println!(
        "host sees /shared/data/report.md ({} bytes)",
        from_host.len()
    );

    // --- The read-only system root ----------------------------------------
    // System content is readable...
    MemFs::write_atomic("/system/skills/builtin.md", b"baked-in skill")?;
    println!(
        "/system/skills/builtin.md -> {:?}",
        String::from_utf8_lossy(&sandbox.read("/system/skills/builtin.md")?)
    );
    // ...but never writable from inside the sandbox.
    report(
        "write /system/skills/x",
        sandbox.write_atomic("/system/skills/x", b"nope"),
    );

    // --- Rejected paths ----------------------------------------------------
    // Bare roots that were never listed as visible.
    report("list /shared", sandbox.list_dir("/shared"));
    // Anything outside the visible roots.
    report("read /etc/passwd", sandbox.read("/etc/passwd"));
    // A `..` traversal that tries to climb out of the sandbox.
    report(
        "read /sandbox/../../etc/passwd",
        sandbox.read("/sandbox/../../etc/passwd"),
    );

    Ok(())
}

/// Print how an operation that is expected to be denied turned out.
fn report<T>(what: &str, outcome: Result<T, SandboxError>) {
    match outcome {
        Ok(_) => println!("{what}: unexpectedly allowed"),
        Err(error) => println!("{what}: denied -> {error}"),
    }
}
