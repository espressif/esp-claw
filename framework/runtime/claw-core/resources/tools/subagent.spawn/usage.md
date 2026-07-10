Delegate a self-contained task to a new specialist subagent instead of doing it
inline. Pick the `kind` best suited to the goal and write the `goal` as a
complete, standalone brief — the subagent does not see your conversation.

Give the subagent a short, required `name` so you can tell several subagents apart
in `subagent.list` / `subagent.watch`. The name is just a label; you still
`subagent.delete` (and refer to it) by the agent id this call returns.

The subagent runs on its own and reports its result back to you when it finishes;
integrate that result as it arrives. Use `termination` to choose its lifecycle:

- `auto` (default) — one-shot: the subagent is removed as soon as it reports.
- `manual` — persistent: it stays alive and idle after reporting, so you can
  watch it, hand it more work, or delete it later. You are then responsible for
  removing it with `subagent.delete` when it is no longer needed.
