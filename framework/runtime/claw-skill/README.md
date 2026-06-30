# claw-skill

Skill registry and per-agent loaded set for the ESP-Claw agent framework.

A *skill* is a dynamically loadable chunk of prompt context. Each skill lives in
its own directory under a skills root and is described by a single `SKILL.md`
file. `claw-skill` scans those directories into a cheap **catalog**, lets an
agent **load/unload** skills at runtime, and assembles the loaded skill
documents into a borrowed prompt-context fragment — all over an injected
filesystem (`ClawFs`), so the same code runs on-device and in host tests.

## `SKILL.md` layout

A skill directory is `<root>/<id>/SKILL.md`, where the id is the directory name.
The file head is a JSON front-matter block fenced by `---` lines, followed by a
markdown body:

```text
---
{ "description": "Turn board lights and LED strips on or off." }
---
# Light switch
Call the light capability ...
```

Only `description` is read into the catalog; the id comes from the directory
name. Any other front-matter keys are ignored. The catalog scan reads only a
bounded prefix of each file (front-matter is a few hundred bytes), so large
bodies are never touched until a skill is actually placed in context.

## Public API

Re-exported from the crate root:

| Type | Role |
|------|------|
| `SkillId` | A skill's identity (its directory name). `Cow<'static, str>`-backed, so a build-baked id (`from_static`, `const`) and a runtime id (`new`) share one type and compare equal by content. |
| `SkillMetadata` | A cheap catalog row: `id()` + `description()`, no document body. |
| `SkillRegistry` | Trait: the catalog + document source. `catalog()` (returns a shared `Arc<CatalogSnapshot>`), `write_document()`, `document()`, `metadata()`. |
| `CatalogSnapshot` | An immutable point-in-time view of the catalog: `entries()` + `get()`. Handed out via `Arc` so a concurrent `reload()` can't mutate a reader's view. |
| `FsSkillRegistry` | `SkillRegistry` backed by one or more `ClawFs` skills roots: `scan()`, `scan_roots()`, `reload()` (re-scans and atomically swaps in a fresh snapshot, on `&self`). |
| `SkillSet` | An agent's loaded skills + two dirty-cached, borrowed prompt fragments: `catalog()` (the available-skills menu) and `context()` (the loaded bodies). Mutable at runtime: `load()`, `load_group()`, `unload()`. |
| `SkillGroup` | A named bundle of skill ids to load together. The name tags provenance in the assembled context. |
| `SkillError` | Failure enum: `ScanFailed`, `DuplicateId`, `ReadFailed`, `InvalidUtf8`, `MissingOpeningFence`, `MissingClosingFence`, `InvalidJson`, `NotFound`. |

### Design notes

- **Ids are unique across all roots.** Scanning multiple roots merges their
  catalogs; the same id in two roots is a hard `SkillError::DuplicateId`, not a
  silent override.
- **Allocation-frugal context.** `SkillRegistry::write_document` appends a
  body straight into a caller-owned buffer, and `SkillSet` reuses one buffer
  across rebuilds — no `String` per document per rebuild.
- **Dirty-cached fragments.** `catalog()` is cached and keyed on the registry's
  current snapshot identity (a `reload()` invalidates it); `context()` is rebuilt
  only when the loaded set changes, so steady-state reads are O(1) and hand back a
  borrowed `&str`.

## Usage

```rust
use std::sync::Arc;

use claw_interface::ClawFs;
use claw_skill::{FsSkillRegistry, SkillGroup, SkillId, SkillRegistry, SkillSet};

fn build(fs: Arc<dyn ClawFs>) -> anyhow::Result<()> {
    // Scan one or more skills roots into a catalog (only front-matter is read).
    let registry: Arc<dyn SkillRegistry> = Arc::new(FsSkillRegistry::scan(fs, "skills")?);

    // The available-skills menu shown to the model.
    let mut set = SkillSet::new(registry);
    println!("{}", set.catalog());

    // Load skills at runtime — individually or as a named group.
    set.load("manual", SkillId::new("light_switch"))?;
    set.load_group(SkillGroup::new("hardware", [SkillId::new("board_hardware_info")]))?;

    // Assemble the loaded bodies into one borrowed prompt fragment (cached).
    println!("{}", set.context()?);

    // Unload without restarting the agent.
    set.unload(&SkillId::new("light_switch"));
    Ok(())
}
```

## Examples

Runnable on the host with an in-memory `MemFs`:

```bash
cargo run --example catalog       --target x86_64-unknown-linux-gnu
cargo run --example load_context  --target x86_64-unknown-linux-gnu
cargo run --example multi_root    --target x86_64-unknown-linux-gnu
```

## Where it fits

`claw-skill` is a pure-Rust core crate: it depends only on the `ClawFs` trait
from `claw-interface`, never on a platform directly, so it is fully
host-testable. In the firmware, `claw_core` wraps an `FsSkillRegistry` over the
on-device filesystem and exposes per-agent load/unload through `BaseAgent`
(skills declared in a generated manifest are loaded under the `"manifest"`
group).
