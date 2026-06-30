# Context Model Specification

`claw-context` assembles the context handed to the LLM on every iteration. This
is the **authoritative definition**; implementation and tests follow it strictly.
It is a full overhaul of the legacy three-bucket layout, with no backward
compatibility. Goal: prefix-cache hit rate, token efficiency, and LLM quality
across generalized agents.

## Architecture vs Layout

Two separate concerns; conflating them poisons the cache:

- **Architecture** — how context is organized, owned, and sourced (Part A). Not a
  byte order.
- **Layout** — the byte order sent to the LLM, sorted to maximize the cached
  prefix (Part B). Sorted by **mutability**, not scope.

Every block has a **scope** (architecture) and a **mutability class** (layout).
Layout reads *only* mutability; scope is a secondary tiebreaker within a tier.
Example: `AgentInstruction` is agent-scoped but immutable, so it sits at the top
of the wire with `CommonInstruction`, *above* the global-scoped but mutable `Soul`
and `GlobalMemory`. Mutability moved it, not scope.

## Realization: the two wire fields

The model above is unchanged; this is only how it maps onto the request the API
client sends. A request has exactly two wire fields, and `Context` is the single
assembler that produces both (`Context::request(history)` returns a
`RequestContext` of `system` + the two tail segments):

- **`system` (prefix)** — the cacheable prose `Block`s (Bands 1–2, the durable
  prefix) declared via `Context::with` and rendered into one string in a reused
  buffer, re-rendered only when a block actually changes (gated by
  `Context::version`).
- **`messages` (tail)** — the Band-3 structured tail, as **two segments kept
  separate** so appending never clones history:
  - the persisted conversation `history` (`ConversationSummary` +
    `RecentMessages/…` + `CurrentInput`), owned by memory; and
  - ephemeral **reminders** — per-request nudges (e.g. a soft-hide phase note,
    or a static-but-last `OutputContract` realized as a trailing
    `<system-reminder>`) that are **never persisted**.

Determinism rule (one home per item): stable prose by scope -> a `Block`
(prefix); a real, persisted conversation/tool event -> memory `history` (tail);
a per-request transient nudge -> `reminders` (tail). There is no fourth option.

---

# Part A — Architecture

## Block Groups

A conceptual grouping by responsibility — not the wire order, not the scope
nesting.

```
Context
├── Core        CommonInstruction · Soul · AgentInstruction · ToolPolicy · ActiveSkills
├── Mode        ConversationModeContext | WorkingModeContext
├── Knowledge   GlobalMemory/SessionMemory/AgentMemory (push) · PulledKnowledge (pull)
├── History     ConversationSummary · RecentMessages/Events/ToolResults/Errors/Approvals
├── Input       CurrentInput
└── Output      OutputContract
```

## Mode Model (the primary extension axis)

`ModeContext` lets one model serve different agent behaviors without
restructuring. Modes are **never mixed** — an agent pays only for its mode.

- **ConversationMode** — dialogue: answer, clarify, route. No task scaffolding.
- **WorkingMode** — task execution: `RunContext` / `TaskSpec` / `WorkspaceContext`
  (stable framing) plus `WorkingState` / `ApprovalState` / `Blockers` (live state).

On the wire, mode splits by mutability (framing → Band 2, live state → Band 3),
but it is one architectural concept. *Future modes* slot in with no band change:
`Planning`, `Review`, `Approval`, `MemoryUpdate`, `Device`, `ToolExecution`.

## Memory: Three Axes

Memory feels chaotic because each artifact has a value on three independent axes;
naming the artifact hides two of them.

| Axis | Question | Values |
|---|---|---|
| **Scope** | Whose is it / how widely shared? | Global / Session / Agent / Conversation |
| **Injection** | How does it reach the model? | Push (prefix) / Pull (tool → tail) |
| **Mutability** | How often does it change? | Immutable / Durable-mutable / Volatile |

Artifacts are combinations:

| Artifact | Scope | Injection | Mutability |
|---|---|---|---|
| `common_instruction.md` | Global | Push | Immutable |
| `agents/<role>/instruction.md` | Agent | Push | Immutable |
| ToolPolicy prose | Agent | Push | Immutable |
| `soul.md` | Global/Agent | Push | Durable-mutable |
| `MEMORY.md` (one per level) | Global/Session/Agent | Push | Durable-mutable |
| `ConversationSummary` | Conversation | Push | Durable-mutable |
| long-term memory, `RetrievedDocs` | any | Pull | result is Volatile |

## Scope Nesting (ownership, not layout)

```
Global ⊃ Session ⊃ Agent ⊃ Conversation ⊃ Turn
```

A **Session contains agents**; an **Agent** exists only within its session; a
**Conversation** is one agent's dialogue. Scope governs *reuse direction* (who
can share a cached span) and is the secondary sort within a mutability tier — it
never overrides mutability.

## Push vs Pull

- **Push** — durable, whole content carried by scope → the **prefix**.
- **Pull** — query-specific knowledge fetched per iteration → `RecentToolResults`
  in the **tail**.

> **PulledKnowledge** — anything retrieved per-query: long-term memory,
> `RetrievedDocs`, repo / API / external lookups. Always lands in the tail.

**Corpus-scope ≠ result-volatility.** A retrieved *result* is always volatile
even when its corpus is global: push the durable *whole* by scope; pull the
query-specific *slice* into the tail.

## Block Catalog

Group, scope, source, and extension points. (Mutability / band in Part B.)

- **CommonInstruction** — *Core, Global.* `common_instruction.md`. *Extends:*
  `safety` / `runtime` / `style` (don't over-split first).
- **Soul** — *Core, Global or Agent.* Persona/identity. `soul.md`.
- **AgentInstruction** — *Core, Agent.* Role and boundaries.
  `agents/<role>/instruction.md`. *Extends:* frontend / worker / reviewer /
  planner / memory_writer.
- **ToolPolicy** — *Core, Agent.* **Not** tool schema (schema → API `tools`).
  Prose: capability classes, when to use tools, what needs approval, never
  fabricate results, when to pull. Live permission state is `ApprovalState` (tail),
  not here. *Extends:* filesystem / network / hardware / approval / sandbox / risk
  policies.
- **ActiveSkills** — *Core, Agent.* Activated `SKILL.md` text only; metadata
  (`skill.toml`) is router-only. *Extends:* 0–N active; full/degraded/short modes.
- **ModeFraming** — *Mode, Agent.* Stable half of `ModeContext` (see Mode Model).
- **GlobalMemory / SessionMemory / AgentMemory** — *Knowledge.* `MEMORY.md` per
  scope, pushed whole. *Extends:* `team_memory` / `device_docs` / `hardware_specs`.
- **SessionContext** — *Knowledge, Session.* Session-wide shared framing, if any.
- **PulledKnowledge** — *Knowledge, pull.* Long-term memory, `RetrievedDocs`,
  repo/API lookups. Results land in the tail.
- **ConversationSummary** — *History, Conversation.* Compressed dialogue. *Extends:*
  short / detailed / per-topic.
- **RecentContext / LiveState** — *History/Mode, Turn.* Recent raw
  messages/events/results; `LiveState` = volatile half of mode.
- **CurrentInput** — *Input, Turn.* Verbatim; never dropped or compressed.
- **OutputContract** — *Output, Agent/mode.* Conversation: NL answer/style.
  Working: structured JSON (`actions`/`blockers`/`needs_approval`/`memory_updates`/
  `next_step`). *Extends:* per-agent/mode contracts.

---

# Part B — Layout (Wire Order)

## Sorting Rule

1. **Mutability (primary):** Immutable → Durable-mutable → Volatile. Nothing
   mutable ever sits above something immutable.
2. **Scope (secondary):** within a tier, broad → narrow. In the durable tier this
   also tracks mutation frequency (broader = rarer) and aids cross-entity reuse.
3. **Determinism:** a block renders identical bytes when its inputs are unchanged
   (no map-iteration order, no incidental timestamps). The cache keys on bytes.

## The Bands

```
BAND 1 — STATIC INSTRUCTIONS   (immutable; the long shared prefix, never busted at runtime)
  CommonInstruction · AgentInstruction · ToolPolicy

BAND 2 — DURABLE STATE         (slowly mutable; broad→narrow scope; an edit busts only Bands 2–3)
  Soul · GlobalMemory · SessionContext · SessionMemory · AgentMemory
  ActiveSkills · ModeFraming · ConversationSummary

BAND 3 — VOLATILE TAIL         (rebuilt each iteration; append-only between compactions)
  RecentContext + LiveState (RecentMessages/Events/ToolResults/Errors/Approvals;
      WorkingState/ApprovalState/Blockers; PulledKnowledge results land here)
  CurrentInput
  OutputContract               (static, but last by exception — see below)
```

Band 3 is append-only between compactions, so each iteration adds tokens only at
the end and the whole prefix stays cached. Pulled knowledge lands next to
`CurrentInput`, in the high-attention zone.

## Exceptions

- **`OutputContract`** — static but emitted last: it won't cache (volatile tail
  precedes it), but recency improves instruction following, and it's tiny.
- **`ModeContext`** — split by mutability: framing → Band 2, live state → Band 3.
- **Time / run metadata** — volatile; Band 3 only.

## Cache Breakpoints

Provider breakpoints (Anthropic: up to 4) go after **Band 1** and within **Band 2**
after the Global and Session sub-groups, so a change reuses every region above it.
Regions below the provider minimum (~1024 tokens on OpenAI) won't cache alone.

## Block Attribute Map

| Block | Scope | Mutability | Band |
|---|---|---|---|
| CommonInstruction | Global | Immutable | 1 |
| AgentInstruction | Agent | Immutable | 1 |
| ToolPolicy | Agent | Immutable | 1 |
| Soul | Global/Agent | Durable-mutable | 2 |
| GlobalMemory | Global | Durable-mutable | 2 |
| SessionContext / SessionMemory | Session | Durable-mutable | 2 |
| AgentMemory | Agent | Durable-mutable | 2 |
| ActiveSkills | Agent | Durable-mutable | 2 |
| ModeFraming | Agent | Durable-mutable | 2 |
| ConversationSummary | Conversation | Durable-mutable | 2 |
| RecentContext / LiveState / PulledKnowledge | Turn | Volatile | 3 |
| CurrentInput | Turn | Volatile | 3 |
| OutputContract | Agent/mode | Static (last, by exception) | 3 |

## Extension Invariant

**Do not add or reorder bands.** Extend within a band (new memory scope,
sub-policy, knowledge corpus, or `ModeContext` variant). Classify a new source by
the three axes, then place it by mutability first, scope second. Never put mutable
content above Band 1.

## Open Decisions

Product calls; each lists the default the layout assumes.

1. **Soul scope** — default Global. Band 2 either way; scope only shifts position
   within Band 2.
2. **Memory write cadence** — default: written via tool at a boundary (write lands
   in tail; injected copy refreshes next turn → stable-within-turn). A live
   per-iteration scratchpad is Volatile and moves to Band 3.
3. **`RetrievedDocs` injection** — default *pull* (tail). For always-on grounding,
   it becomes a push block at the bottom of Band 2 (one volatile-ish prefix block).
4. **ActiveSkills vs ModeFraming order** in Band 2 — default `ActiveSkills` first;
   swap if framing proves more stable.
5. **`SessionContext`** — confirm what session-wide framing exists beyond
   `SessionMemory`, or drop it.

## Relationship to the LLM Request

All blocks are prose / structured-text context. **Tool schemas are not part of
this model** — they go in the API `tools` field. `ToolPolicy` governs *behavior*
and *when to pull*; the schema governs *shape*.
