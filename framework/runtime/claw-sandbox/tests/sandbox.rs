#![allow(clippy::unwrap_used)]

use claw_interface::{ClawFs, MemFs};
use claw_sandbox::{RealRoots, Sandbox, SandboxError, SandboxFs};

const REAL: RealRoots = RealRoots {
    shared_skills: "/real/shared/skills",
    shared_tmp: "/real/shared/tmp",
    shared_data: "/real/shared/data",
    system_skills: "/real/system/skills",
};

#[test]
fn routes_each_visible_root_to_its_real_path() {
    MemFs::new();
    let sb = Sandbox::<MemFs>::new("/host/sandbox", REAL).unwrap();

    sb.write_atomic("/sandbox/tmp/a", b"1").unwrap();
    sb.write_atomic("/shared/data/b", b"2").unwrap();

    assert_eq!(MemFs::read("/host/sandbox/tmp/a").unwrap(), b"1");
    assert_eq!(MemFs::read("/real/shared/data/b").unwrap(), b"2");
}

#[test]
fn scratch_dirs_are_listable_after_construction() {
    let sb = sandbox();
    assert_eq!(
        sb.list_dir("/sandbox/skills").unwrap(),
        Vec::<String>::new()
    );
    assert_eq!(sb.list_dir("/sandbox/tmp").unwrap(), Vec::<String>::new());
}

#[test]
fn bare_shared_and_system_roots_are_rejected() {
    let sb = sandbox();
    assert!(matches!(
        sb.list_dir("/shared"),
        Err(SandboxError::OutsideSandbox(_))
    ));
    assert!(matches!(
        sb.read("/system/secret"),
        Err(SandboxError::OutsideSandbox(_))
    ));
    assert!(matches!(
        sb.read("/etc/passwd"),
        Err(SandboxError::OutsideSandbox(_))
    ));
}

#[test]
fn sandbox_root_itself_is_accessible() {
    let sb = sandbox();
    assert!(sb.list_dir("/sandbox").is_ok());
}

#[test]
fn dotdot_escape_is_rejected() {
    let sb = sandbox();
    assert!(matches!(
        sb.read("/sandbox/../../etc/passwd"),
        Err(SandboxError::OutsideSandbox(_))
    ));
}

#[test]
fn dotdot_within_visible_area_is_allowed() {
    let sb = sandbox();
    sb.write_atomic("/sandbox/tmp/../skills/x", b"ok").unwrap();
    assert_eq!(sb.read("/sandbox/skills/x").unwrap(), b"ok");
}

#[test]
fn system_root_is_read_only() {
    let sb = sandbox();
    assert!(matches!(
        sb.write_atomic("/system/skills/x", b"nope"),
        Err(SandboxError::ReadOnly(_))
    ));
    assert!(matches!(
        sb.remove("/system/skills/x"),
        Err(SandboxError::ReadOnly(_))
    ));
}

#[test]
fn system_root_is_readable() {
    MemFs::new();
    MemFs::write_atomic("/real/system/skills/doc", b"hi").unwrap();
    let sb = Sandbox::<MemFs>::new("/host/sandbox", REAL).unwrap();
    assert_eq!(sb.read("/system/skills/doc").unwrap(), b"hi");
}

#[test]
fn exists_distinguishes_absent_from_inaccessible() {
    let sb = sandbox();
    assert!(!sb.exists("/sandbox/tmp/missing").unwrap());
    assert!(matches!(
        sb.exists("/nope/x"),
        Err(SandboxError::OutsideSandbox(_))
    ));
}

fn sandbox() -> Sandbox<MemFs> {
    MemFs::new();
    Sandbox::<MemFs>::new("/real/sandbox/inst-1", REAL).unwrap()
}
