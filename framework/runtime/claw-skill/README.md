# claw-skill

Filesystem-backed skill catalog and one-shot document activation for ESP-Claw.

A skill lives at `<root>/<id>/SKILL.md`. `FsSkillRegistry` scans one or more
priority-ordered roots, keeps a metadata catalog snapshot in memory, and reads
full documents only when a `SkillSet` activates a skill. Activation returns an
owned `SkillDocument`; it does not create persistent loaded-skill state.

## `SKILL.md`

The front-matter is JSON fenced by `---`:

```text
---
{
  "name": "light_switch",
  "description": "Turn a board light on or off.",
  "metadata": {
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly"
  }
}
---
# Light switch
Call the light capability ...
```

Required top-level fields: `name`, `description`, and `metadata`. `author` is
optional. `metadata.manage_mode` accepts `readonly`, `web`, or `runtime`; device
runtime normalizes `web` to `readonly`. `metadata.cap_groups`,
`category`, `peripherals`, and `tags` are parsed and retained.

## API Shape

| Type | Role |
|------|------|
| `FsSkillRegistry` | FS-backed catalog source. Build with `FsSkillRegistry::<F>::new().set_root(data)?.set_root(system)?`; roots are priority ordered. |
| `SkillRegistry` | Minimal resolver-facing trait whose public operation is `skill_set()`. |
| `CatalogSnapshot` | Immutable versioned catalog with `Arc<[Skill]>`. Internal registry scans swap in a new snapshot. |
| `Skill` | One catalog row: id/name/description/author/metadata/document file. |
| `SkillSet` | Per-agent cache and tool surface: `catalog_context()`, `list_skill()`, `activate_skill()`, `reload()`. |
| `SkillDocument` | Owned activated document snapshot. |

## Usage

```rust
use std::sync::Arc;

use claw_interface::MemFs;
use claw_skill::{FsSkillRegistry, SkillId};

fn build() -> Result<(), claw_skill::SkillError> {
    let registry = Arc::new(
        FsSkillRegistry::<MemFs>::new()
            .set_root("data/skills")?
            .set_root("system/skills")?,
    );
    let mut skills = registry.skill_set();

    println!("{}", skills.catalog_context());
    println!("{}", skills.list_skill()?);

    let document = skills.activate_skill(&SkillId::new("light_switch"))?;
    println!("{}", document.content());
    Ok(())
}
```

`activate_skill()` strips front-matter, expands `{CUR_SKILL_DIR}`, and wraps the
body as:

```xml
<skill_content name="light_switch">
...
</skill_content>
```

## Notes

- Skills are filesystem-owned. This crate does not write, register, unregister,
  load, unload, enable, or disable skills.
- DATA roots should be added before SYSTEM roots so user-installed skills shadow
  firmware-baked skills with the same id.
- `SkillSet` owns reusable `catalog_buffer` and `document_buffer`. Share one
  `SkillSet` between the context adapter and skill tools, typically behind a
  mutex, so both paths use the same buffers.
- `reload()` re-scans the registry roots after external filesystem changes; a
  failed reload leaves the previous snapshot in place.
