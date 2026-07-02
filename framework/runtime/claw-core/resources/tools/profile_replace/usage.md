Replace a global profile document with `profile_replace` only when the user asks
to change durable behavior, assistant identity, or their stable profile. This is
a whole-document replacement: preserve relevant existing content when making a
small edit. Do not use this for normal remembered facts; use memory tools for
facts that should be recalled later.
