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
of the wire *above* the global-scoped but mutable `Soul` and `GlobalMemory`.
Mutability moved it, not scope.

## Realization: the two wire fields

The model above is unchanged; this is only how it maps onto the request the API
client sends. A request has exactly two wire fields, and `Context` is the single
assembler that produces both (`Context::request(history)` returns a
`RequestContext` of `system` + the two tail segments). Runtime providers can
feed the assembler through `ContextItem`s accepted by a `ContextSink`: `Block`
items land in `system`, `Message` items land in ordered history, and `Reminder`
items land in the ephemeral reminder tail.

- **`system` (prefix)** — the cacheable prose `Block`s (Bands 1–2, the durable
  prefix) declared via `Context::with` and rendered into one string in a reused
  buffer, re-rendered only when a block actually changes (gated by
  `Context::version`).
- **`messages` (tail)** — the Band-3 structured tail, as **two segments kept
  separate** so appending never clones history:
  - the persisted conversation `history` (`ConversationSummary` +
    `RecentMessages/…`), owned by memory. The active user input is the current
    user message/open turn in this history lane, not a duplicated block; and
  - ephemeral **reminders** — per-request nudges (e.g. `ToolReminder`, the
    soft-hide phase note, or a static-but-last `OutputContract` realized as a
    trailing `<system-reminder>`) that are **never persisted**.

Determinism rule (one home per item): stable prose by scope -> a `Block`
(prefix); a real, persisted conversation/tool event -> memory `history` (tail);
a per-request transient nudge -> `reminders` (tail). Tool-retrieved knowledge
(`memory_recall`, document lookup, web/API calls) is a tool result event in
history, not a separate `PulledKnowledge` block. There is no fourth option.

---

# Part A — Architecture

## Block Groups

A conceptual grouping by responsibility — not the wire order, not the scope
nesting.

```
Context
├── Core        Soul · AgentInstruction · ToolPolicy · ToolReminder · SkillList
├── Mode        ConversationModeContext | WorkingModeContext
├── Knowledge   GlobalMemory/SessionMemory/AgentMemory (push)
│               retrieved knowledge is tool-call output in History (pull)
├── History     ConversationSummary · RecentMessages/Events/ToolResults/Errors/Approvals
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

## Bake-Time Instructions

Shared common instructions are a **manifest-generation concept**, not a runtime
`BlockKind`. The runtime receives a fully baked agent instruction string.

`claw-core` currently reads:

- `resources/agents/common/instructions.md` — shared preamble for every agent
  kind.
- `resources/agents/<kind>/instructions.md` — the kind-specific instruction.

The build script concatenates them into `AgentManifest.instructions`; runtime
then injects that final text as `BlockKind::AgentInstruction`. Do not add a
runtime common-instruction block unless the shared preamble gains an independent
runtime lifecycle.

## Current Input Location

The current user input is not a `BlockKind`. It lives in the transcript:

1. `BaseAgent::run` / `AppendMessage` appends the text as the open user turn.
2. `RecentMessagesContextAdapter` reads committed turns after the summary cursor
   plus the in-progress open turn.
3. The adapter contributes those messages as `RecentContext` in the history tail.

This keeps the user message in the model's normal conversation channel and
prevents duplicating it in the system prefix. Only systems that cannot represent
the active input as a message should use a custom volatile block.

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
| baked common preamble + `agents/<role>/instruction.md` | Agent | Push | Immutable |
| ToolPolicy prose | Agent | Push | Immutable |
| ToolReminder phase note | Agent | Push reminder | Volatile |
| `soul.md` / `identity.md` / `user.md` | Global | Push | Durable-mutable |
| `MEMORY.md` (one per level) | Global/Session/Agent | Push | Durable-mutable |
| `ConversationSummary` | Conversation | Push | Durable-mutable |
| long-term memory recall, `RetrievedDocs` lookup | any | Pull via tool | tool result is Volatile |

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
- **Pull** — query-specific knowledge fetched per iteration by a model-visible
  tool → tool result messages in the **tail**.

There is no canonical `PulledKnowledge` `BlockKind`. If the model asks a tool for
long-term memory, `RetrievedDocs`, repo/API results, or external lookups, that
content lands as a normal tool result and is then carried by recent history and
summaries. Only system-initiated prefetch that happens without a model tool call
needs a separate volatile injection point; prefer a `Custom` volatile block or a
reminder for that exceptional path.

**Corpus-scope ≠ result-volatility.** A retrieved *result* is always volatile
even when its corpus is global: push the durable *whole* by scope; pull the
query-specific *slice* into the tail.

## Block Catalog

Group, scope, source, and extension points. (Mutability / band in Part B.)

- **Soul** — *Core, Global.* Persona, behavior principles, and style. `soul.md`.
- **AssistantIdentity** — *Core, Global.* Assistant/device role, capabilities, and
  boundaries. `identity.md`.
- **UserProfile** — *Core, Global.* The single user's stable preferences and
  interaction agreements. `user.md`.
- **AgentInstruction** — *Core, Agent.* Role and boundaries.
  Runtime text is `resources/agents/common/instructions.md` folded together with
  `resources/agents/<kind>/instructions.md` at build time. *Extends:* frontend /
  worker / reviewer / planner / memory_writer.
- **ToolPolicy** — *Core, Agent.* **Not** tool schema (schema → API `tools`).
  Prose: capability classes, when to use tools, what needs approval, never
  fabricate results, when to pull. Dynamic phase gating does not live here.
  *Extends:* filesystem / network / hardware / approval / sandbox / risk policies.
- **ToolReminder** — *Core, Agent.* Ephemeral tool phase note (e.g. the current
  soft-hide allow-set), rendered as a reminder tail item. It is not persisted and
  does not move the cached system prefix.
- **SkillList** — *Core, Agent.* Available skill catalog rendered as prompt
  guidance. Full skill documents are returned by skill activation tools.
- **ModeFraming** — *Mode, Agent.* Stable half of `ModeContext` (see Mode Model).
- **ReasoningEffort** — *Mode, Agent.* Per-session orchestration guidance for
  how directly or deliberately the root agent should approach the current turn.
- **GlobalMemory / SessionMemory / AgentMemory** — *Knowledge.* `MEMORY.md` per
  scope, pushed whole. *Extends:* `team_memory` / `device_docs` / `hardware_specs`.
- **SessionContext** — *Knowledge, Session.* Session-wide shared framing, if any.
- **Tool-retrieved knowledge** — *Knowledge, pull.* Long-term memory,
  `RetrievedDocs`, repo/API lookups. There is no block kind; results land as
  tool result messages in the tail.
- **ConversationSummary** — *History, Conversation.* Compressed dialogue. *Extends:*
  short / detailed / per-topic.
- **RecentContext / LiveState** — *History/Mode, Turn.* Recent raw
  messages/events/results; `LiveState` = volatile half of mode.
- **OutputContract** — *Output, Agent/mode.* Conversation: NL answer/style.
  Working: structured JSON (`actions`/`blockers`/`needs_approval`/`memory.updates`/
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
  AgentInstruction · ToolPolicy

BAND 2 — DURABLE STATE         (slowly mutable; broad→narrow scope; an edit busts only Bands 2–3)
  Soul · AssistantIdentity · UserProfile · GlobalMemory · SessionContext · SessionMemory · AgentMemory
  SkillList · ModeFraming · ReasoningEffort · ConversationSummary

BAND 3 — VOLATILE TAIL         (rebuilt each iteration; append-only between compactions)
  ToolReminder                 (dynamic tool phase note, reminder tail)
  RecentContext + LiveState (RecentMessages/Events/ToolResults/Errors/Approvals;
      WorkingState/ApprovalState/Blockers; tool-retrieved knowledge is a ToolResult)
  OutputContract               (static, but last by exception — see below)
```

Band 3 is append-only between compactions, so each iteration adds tokens only at
the end and the whole prefix stays cached. Tool-retrieved knowledge naturally
lands next to the user turn because it is a tool result in recent history.

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
| AgentInstruction | Agent | Immutable | 1 |
| ToolPolicy | Agent | Immutable | 1 |
| ToolReminder | Agent | Volatile reminder | 3 |
| Soul / AssistantIdentity / UserProfile | Global | Durable-mutable | 2 |
| GlobalMemory | Global | Durable-mutable | 2 |
| SessionContext / SessionMemory | Session | Durable-mutable | 2 |
| AgentMemory | Agent | Durable-mutable | 2 |
| SkillList | Agent | Durable-mutable | 2 |
| ModeFraming | Agent | Durable-mutable | 2 |
| ReasoningEffort | Agent | Durable-mutable | 2 |
| ConversationSummary | Conversation | Durable-mutable | 2 |
| RecentContext / LiveState / ToolResults | Turn | Volatile | 3 |
| OutputContract | Agent/mode | Static (last, by exception) | 3 |

## Extension Invariant

**Do not add or reorder bands.** Extend within a band (new memory scope,
sub-policy, knowledge corpus, or `ModeContext` variant). Classify a new source by
the three axes, then place it by mutability first, scope second. Never put mutable
content above Band 1.

## Open Decisions

Product calls; each lists the default the layout assumes.

1. **Memory write cadence** — default: written via tool at a boundary (write lands
   in tail; injected copy refreshes next turn → stable-within-turn). A live
   per-iteration scratchpad is Volatile and moves to Band 3.
2. **`RetrievedDocs` injection** — default *pull* through a tool call, so it is a
   tool result in history. For system-initiated prefetch without a tool call, use
   a `Custom` volatile block or reminder. Do not add a `PulledKnowledge` kind.
3. **SkillList vs ModeFraming order** in Band 2 — default `SkillList` first;
   swap if framing proves more stable.
4. **`SessionContext`** — confirm what session-wide framing exists beyond
   `SessionMemory`, or drop it.

## Legacy C Mapping and Rust Integration Backlog

`master` did not have this `BlockKind` taxonomy. It had three context provider
kinds (`SYSTEM_PROMPT`, `MESSAGES`, `TOOLS`) plus persistence records for user,
assistant, assistant-tool, and tool-result messages. The Rust migration should
preserve those behavioral boundaries while mapping old providers to explicit
blocks:

| Context | Legacy behavior on `master` | Rust status / direction |
|---|---|---|
| Soul / AssistantIdentity / UserProfile | `claw_memory_profile_provider` pushed editable profile files (`user.md`, `soul.md`, `identity.md`) into the system prompt. | Implemented as first-class global blocks backed by `ProfileStore` and `ProfileContextAdapter`. |
| SessionContext | No clear legacy equivalent beyond request metadata such as source channel/chat. | Product decision. Implement only if sessions gain stable shared framing; otherwise drop the block kind. |
| SessionMemory | No durable session-scope `MEMORY.md` equivalent. Legacy Session History was transcript storage, not session memory. | Missing by design. Implement only if we need session-wide durable notes distinct from conversation transcript/summary. |
| ModeFraming | Root/subagent role and subagent type prompts were folded into the agent system prompt by the agent manager. | Mostly absorbed by `AgentInstruction` today. Extract to `ModeFraming` only when conversation/working/review/etc. modes need to swap framing independently of agent identity. |
| OutputContract | No standalone legacy provider. Output expectations were implicit in prompts/tools. | Missing. Prefer a small reminder/tail injection when recency matters; use a block only for stable per-agent or per-mode contracts. |

## Relationship to the LLM Request

All blocks are prose / structured-text context. **Tool schemas are not part of
this model** — they go in the API `tools` field. `ToolPolicy` governs stable
tool-use behavior and *when to pull*; `ToolReminder` governs volatile phase
availability; the schema governs *shape*.
