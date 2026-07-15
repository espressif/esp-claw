List active subagents in your subtree plus short-lived completion records, with
each one's id, kind, and status. Use it to see what is still running before you
watch, retask, or stop a specific subagent.

`completed_pending_delivery` means the subagent has finished and its result is
already queued in its parent's inbox, but the parent is still busy and has not
consumed it yet. It is not a live agent and cannot be retasked or stopped. The
record disappears as soon as the parent activates that inbox; its absence after
that point does not mean the result was lost.
