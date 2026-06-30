# Sandbox

The sandbox presents the agent with a single, unified virtual filesystem. Every
path the agent can see is rooted at one of three top-level prefixes. The prefix
alone determines a path's **lifetime** and **visibility** — i.e. who else can see
it and whether it survives the sandbox being torn down.

## Visible roots

Inside the sandbox, exactly these roots are visible:

```
/sandbox/skills/
/sandbox/tmp/
/sandbox/

/shared/skills/
/shared/tmp/
/shared/data/

/system/skills/
```

**The list above is exhaustive.** Only these exact paths (and their contents)
are accessible. Anything not explicitly listed is neither visible nor
addressable from within the sandbox — this includes parent paths that were not
listed:

- `/shared/` and `/system/` are **not** accessible on their own. They are only
  prefixes for the listed subdirectories; you cannot read, write, or list the
  bare root itself, only the explicit children above (`/shared/skills/`,
  `/shared/tmp/`, `/shared/data/`, `/system/skills/`).
- `/sandbox/` **is** accessible, because it is listed explicitly above.
- Any other path (e.g. `/shared/other/`, `/etc/`, a bare `/`) is rejected.

## Lifetime and visibility by prefix

### `/sandbox/` — private and ephemeral

Scratch space owned by a single sandbox instance.

- **Private**: visible only to the sandbox that created it. No other sandbox and
  nothing outside the sandbox can see it.
- **Ephemeral**: destroyed when the sandbox is torn down. Nothing written here
  survives the sandbox's lifetime.

Use it for per-run working files, intermediate state, and anything that should
disappear together with the sandbox.

- `/sandbox/skills/` — skills installed for this sandbox instance only.
- `/sandbox/tmp/` — scratch / temporary files for this sandbox instance.

### `/shared/` — shared with the outside, persistent

Storage shared between the inside and the outside of the sandbox.

- **Shared**: visible both inside the sandbox and to the host outside it, so it
  is the channel for exchanging data across the sandbox boundary.
- **Persistent**: not tied to a single sandbox's lifetime. Content here outlives
  the sandbox that wrote it and is observable by later sandboxes and by the host.

Use it for results meant to leave the sandbox and for state that must survive
across sandbox runs.

- `/shared/skills/` — skills shared across sandboxes and with the host.
- `/shared/tmp/` — shared scratch space (shared, but still scratch).
- `/shared/data/` — shared persistent data.

### `/system/` — system-provided, read-only

Firmware/system-baked content provided by the platform.

- **System-owned**: provided by the system, not created by any sandbox.
- **Read-only**: the sandbox can read it but must not modify it.

- `/system/skills/` — built-in skills shipped with the system.
