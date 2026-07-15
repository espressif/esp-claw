Get the current state of a subagent by id: its kind and status. You can only
watch a subagent in your own subtree.

A completed background subagent remains temporarily watchable with status
`completed_pending_delivery` while its result waits in the parent's inbox. It
is already stopped and cannot be followed up or deleted. The record is removed
when the parent consumes the result; after that it can no longer be watched.
