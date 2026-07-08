# Trace Format Specification

`claw-log`'s `FlatTreeSubscriber` flattens the tracing span tree into single lines that an offline parser can reconstruct back into a tree. This document is the **authoritative definition**; the `trace.rs` implementation and its unit tests must follow it strictly.

## Line Structure

```
TRACE <timestamp> <type> <tracing-context> <incremental-context>* <custom-context>
```

- Anything before `TRACE` is the transport-layer prefix (ESP_LOG's `I (…) tag:` / the host logger's prefix) and is **not part of this format**; parsing starts at the `TRACE` marker.
- `<timestamp>`: framework-filled **monotonic timestamp (ms, since boot; on host normalized to an equivalent monotonic clock)**, the first token after `TRACE`. The duration of a span is the `<timestamp>` difference between its `exit` and `enter` and **does not depend on the transport prefix**.
- The three layers, and the tokens inside each `<...>`, are separated by a **single space** (no alignment padding); a line never contains a newline (`\n` is converted to a space before emit).
- Each token inside `<...>` is `key=value`, and **neither key nor value may contain a space** (space is the separator). The writer asserts this with `debug_assert!`; anything containing spaces must go in the custom context.
- **All parsable structural information lives inside the `<...>` blocks**; everything after them is the custom context — free text, **not parsed** (spaces / commas / pipes / angle brackets are all allowed).
- A line begins with exactly one tracing-context block, followed (on `enter` only) by **zero or more** incremental-context blocks `<context=group …>`. The parser reads the leading `<...>` blocks; the remainder of the line is the custom context.

## ① `<type>`

`enter` (enter a span) / `exit` (leave a span) / `event` (an instantaneous record inside a span).

## ② tracing-context (framework-filled, one `<...>` block)

Coordinates the framework derives from metadata / stack / thread. Contents are fixed per type:

| type | `<...>` contents |
|------|------|
| `enter` | `span=<id> parent=<id\|none> task=<label> span-name=<name> target=<module>` |
| `exit`  | `span=<id> task=<label>` |
| `event` | `span=<id\|none> task=<label> event-name=<name> target=<module>` |

- `span`/`parent`: a framework-assigned, **monotonically increasing, never-recycled** unique id (not the raw tracing id), globally unique across the whole trace stream, so pairing/tree-building is unambiguous.
- `span`: the id of the span this record belongs to; for an `event` with no enclosing span it is `none` (how the consumer renders an orphan is decided by the visualizer and is out of scope for this spec).
- `parent`: the id of the parent span; the outermost span has no parent and is recorded as `none`.
- `task`: the thread name, taken from the thread API (host `std::thread::name`, ESP FreeRTOS task name), used to disambiguate across threads.
- `enter`/`exit` come **one pair per span**, corresponding to span creation/destruction (not per poll); they are paired by the same `span` (duration = the `<timestamp>` difference), and the span name is looked up by `span` from the `enter` line.

## ③ incremental-context (caller-configured groups, `enter` only)

Inherited context is organized into one or more **named groups**, each a closed, ordered key set. Groups are **not** baked into `claw-log`; the caller registers them at subscriber init:

```rust
TracingConfig::default()
    .with_context_group_keys("run", ["session", "turn", "agent", "iteration"])
```

`claw_core` registers the `run` group above (fixed order `session → turn → agent → iteration`); other subsystems may register their own groups.

Each group that opens at least one key on a span renders as its own block on that span's `enter` line, in registration order:

```
<context=<group> <key>=<value> …>
```

**Call site (how a field becomes incremental context):** a span field named `group.key` (dotted) is routed to that group's context; any other field is custom context.

```rust
info_span!("agent", run.agent = %agent_id, depth = 1);
//                  └─ group=run, key=agent ──┘  └─ custom ─┘
// →  … <context=run agent=agent-1> depth=1
```

- A field's `group` prefix must be a registered group **and** its `key` must be in that group's closed set; a registered prefix with an unknown key is a typo and trips `debug_assert!`. A dotted field whose prefix is **not** a registered group (e.g. `http.method`) is ordinary custom context.
- A key appears **once, on the `enter` line of the span that opens it**; descendant lines / events do **not** repeat it. The consumer reconstructs context via the stack: `enter` pushes, `exit` pops, and the full context of any line = the merged stack (child overrides parent), per group.
- **Prefix-closed (per group)**: because the span hierarchy is a fixed nesting, a group's reconstructed key set is always a prefix of its declared order — e.g. for `run`, `agent` present ⟹ `session`+`turn` present; `iteration` present ⟹ all three present. There is never a "has `agent` but missing `turn`" gap.
- A subagent re-opening `run.agent` is a shadow that overrides its subtree, reverted on `exit`.
- A group that opens no key on a span emits **no block** (no empty `<context=…>`). `event` lines carry no incremental block at all.

## ④ custom-context (call site, free-form)

Each span/event's own content — **developer-defined, no format requirement, no `|`** — appended verbatim after the `<...>` blocks; the framework does not parse it.

- Only `enter` (span creation arguments) and `event` (record content) may carry a custom context.
- An `exit` line has only the tracing context (`span=<id> task=<label>`); it carries **neither incremental nor custom context**.

## Span Hierarchy

`session` (opens `session`) > `turn` (opens `turn`) > `agent` (opens `agent`) > `iteration_loop` (opens `iteration`).

- span = a unit of work with a start and end (`enter`/`exit` paired); event = an instantaneous fact.

## Example (with subagent shadow)

```
TRACE 2100 enter <span=1 parent=none task=main span-name=session target=claw_core::orchestrator> <context=run session=session-1>
TRACE 2105 enter <span=2 parent=1 task=main span-name=turn target=claw_core::orchestrator> <context=run turn=7> message_id=m1 cause=message
TRACE 2110 enter <span=3 parent=2 task=main span-name=agent target=claw_core::agent::registry> <context=run agent=agent-1> kind=conversation depth=0
TRACE 2112 enter <span=4 parent=3 task=main span-name=iteration_loop target=claw_core::iteration_loop> <context=run iteration=iteration-0>
TRACE 2120 enter <span=5 parent=4 task=main span-name=agent target=claw_core::agent::registry> <context=run agent=agent-2> kind=tool depth=1
TRACE 2121 event <span=5 task=main event-name=spawned target=claw_core::agent::registry> parent_agent=agent-1 child_agent=agent-2
TRACE 2130 exit <span=5 task=main>
TRACE 2150 event <span=4 task=main event-name=completion target=claw_core::iteration_loop> status=done 👋 Hello!
TRACE 2152 exit <span=4 task=main>
TRACE 2154 exit <span=3 task=main>
TRACE 2156 exit <span=2 task=main>
TRACE 2158 exit <span=1 task=main>
```

- The `event-name=spawned` on `span=5` carries no incremental block; the merged `run` stack reconstructs it as `session-1 + turn-7 + agent-2 + iteration-0`. The `event-name=completion` on `span=4` reconstructs as `session-1 + turn-7 + agent-1 + iteration-0`.
