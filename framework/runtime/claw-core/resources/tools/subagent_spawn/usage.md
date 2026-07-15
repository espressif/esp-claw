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
  running and its result is delivered asynchronously to the spawning parent.
  If the parent is already running, the result waits until its next safe agent
  boundary and `subagent_list` / `subagent_watch` report the finished child as
  `completed_pending_delivery` during that interval.

Always set the required `timeout_ms` to the maximum lifetime this delegated work
may consume. The deadline covers the subagent and the complete subtree it owns.
If it expires, the runtime reports one failed result to the spawning parent and
atomically deletes that entire subtree. The deadline continues while an agent is
idle or waiting for permission input. If a persistent session is restored after
a runtime restart, each live subagent receives a fresh full `timeout_ms` window;
process-local elapsed time is not checkpointed.

Every subagent is one-shot. A subagent that spawned background children is kept
idle after a successful response until those children report back; each result
resumes it through its inbox. It is removed only after it has no live children
and reports its final result. While a background subagent is still live, you can
inspect, retask, or stop it with the other subagent tools.
