# Component Skill Build Rules

This document defines how firmware components contribute built-in Skills to the SYSTEM filesystem image. For the package format shared with runtime-installed Skills, see [`skills/skill_creator/references/skill-package-spec.md`](skills/skill_creator/references/skill-package-spec.md).

## Source Layout

Any build component may provide a `skills/` directory:

```text
component_xx/
└── skills/
    └── skill_id/
        ├── SKILL.md
        ├── references/
        ├── scripts/
        └── assets/
```

The complete `skills/<skill_id>/` directory belongs to that Skill and is copied unchanged to `skills/<skill_id>/` in the application SYSTEM filesystem image.

## Build Synchronization

During the build, `tools/sync_component_skills.py` scans the `skills/` directory of every build component.

- Every `skills/<skill_id>/SKILL.md` must exist.
- Every Skill id and output file path must be unique across the project.
- Package subdirectories retain their relative paths.
- A conflict causes the build to fail instead of silently replacing a Skill.
- Files previously recorded in the build manifest are removed from the output when their component source files no longer exist.

Source paths are component-relative:

```text
component_xx/skills/light_switch/SKILL.md
component_xx/skills/light_switch/scripts/switch.lua
```

Generated paths are relative to the SYSTEM filesystem image:

```text
skills/light_switch/SKILL.md
skills/light_switch/scripts/switch.lua
```

Skill documents must still use `{CUR_SKILL_DIR}` at runtime; they must not depend on either source-tree or generated-image relative paths.
