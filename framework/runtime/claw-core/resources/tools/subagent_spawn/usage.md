Delegate a self-contained task to a new specialist subagent instead of doing it
inline. Pick the `kind` best suited to the goal and write the `goal` as a
complete, standalone brief — the subagent does not see your conversation.

Give the subagent a short, required `name` so you can tell several subagents apart
in `subagent_list` / `subagent_watch`. The name is just a label; you still
`subagent_delete` (and refer to it) by the agent id this call returns.

Choose the required execution mode explicitly:

- `foreground: true` — wait; this tool call returns the subagent result in the
  current turn.
- `foreground: false` — return the agent id immediately; the subagent keeps
  running and its result starts a later subagent-origin turn.

Every subagent is one-shot and is removed automatically after reporting its
result. While a background subagent is still running, you can inspect, retask,
or stop it with the other subagent tools.
