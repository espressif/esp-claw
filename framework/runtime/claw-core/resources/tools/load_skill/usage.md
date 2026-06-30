Load one skill (by `skill` id from `list_skills`) into your context. Its full
guidance becomes visible from the next turn on. Loading a skill that is already
loaded is a no-op; an unknown id is rejected, so call `list_skills` first if you
are unsure. The id is matched against a cached catalog — for a skill just added
to disk, run `reload_skills` first.
