Return the available skill catalog as JSON. Reads the cached registry snapshot;
if a skill was just added, removed, or edited on disk, call `skill.reload`
first. Use the returned `id` as `skill_id` for `skill.activate`.
