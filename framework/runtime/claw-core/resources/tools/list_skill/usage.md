Return the available skill catalog as JSON. Reads the cached registry snapshot;
if a skill was just added, removed, or edited on disk, call `reload_skills`
first. Use the returned `id` as `skill_id` for `activate_skill`.
