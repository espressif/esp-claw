Report a human decision on a subagent's pending approval. Subagents cannot talk
to the user, so their approval requests surface to you as a message like
`[approval request from agent-N] <summary>`. Present it to the user, read their
reply, and classify it for that subagent:

- `verdict: "yes"` — the user clearly approves.
- `verdict: "no"` — the user clearly declines; put the reason in `note`.
- `verdict: "other"` — anything that is not a clear yes or no (a question, a
  request to change the plan, a partial objection). Treated as a decline; pass
  the user's own words in `note` so the subagent can reconsider.

Always pass the exact `agent` id from the request (e.g. `agent-N`).
