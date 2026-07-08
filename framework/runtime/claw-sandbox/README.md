# claw-sandbox

The sandbox filesystem the agent runs inside.

`claw-sandbox` confines all agent file access to a fixed set of **virtual
roots** and rejects anything outside them. It wraps an injected
`claw_interface::ClawFs` backing store, so the exact same confinement applies
on-device (over FATFS / SD) and in host tests (over an in-memory `MemFs`).

## Virtual roots

| Virtual root | Visibility | Writable | Purpose |
|---|---|---|---|
| `/sandbox/*` | private, per-instance | yes | ephemeral scratch for one run |
| `/shared/{skills,tmp,data}/*` | shared with the host | yes | results handed across the sandbox boundary |
| `/system/skills/*` | system-provided | no (read-only) | firmware-baked content |

Each visible virtual root is mapped onto a real path in the backing store; the
private `/sandbox` root maps to a per-instance host directory. Everything
else — bare roots (`/shared`), unlisted paths (`/etc/passwd`), and `..`
traversals that try to climb out — is rejected with a `SandboxError`.

## Public API

Re-exported from the crate root:

| Item | Role |
|---|---|
| `Sandbox` | The sandbox over a backing `ClawFs`: `Sandbox::<F>::new(instance_dir, RealRoots { .. })`. |
| `RealRoots` | The real backing paths each shared/system virtual root maps to (`shared_skills`, `shared_tmp`, `shared_data`, `system_skills`). |
| `SandboxFs` | The confined filesystem surface (read / write / list / … over virtual paths), mirroring `ClawFs`. |
| `SandboxError` | Why an access was denied or failed. |
| `VISIBLE_PREFIXES` / `READ_ONLY_PREFIXES` | The allow-list prefixes that define what is visible and what is read-only. |

## Example

A runnable walkthrough of what is and isn't reachable from inside a sandbox:

```bash
cargo run --example sandbox_demo -p claw-sandbox
```

It writes to the private and shared roots, shows where each write really lands
in the backing store, reads (but cannot write) the read-only system root, and
demonstrates that bare roots, outside paths, and `..` escapes are all denied.

## Where it fits

`claw-sandbox` is a pure-Rust crate depending only on `claw-interface` (the
`ClawFs` seam) and `thiserror`; it bundles into the firmware's `claw_rt`
staticlib and is fully host-testable with the in-memory `MemFs` double.
