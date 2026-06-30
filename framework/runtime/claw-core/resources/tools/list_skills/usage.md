List the skills available to load, each with a one-line description of what it is
for. Reads a cached snapshot (fast, no disk scan); if a skill was just added to
disk, call `reload_skills` first. Use it to pick the right `skill` id for
`load_skill` instead of guessing — anything it returns can be loaded.
