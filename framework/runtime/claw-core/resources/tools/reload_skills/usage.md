Re-scan the skills directory from disk and refresh the catalog. `list_skills` and
`load_skill` read a cached snapshot for speed and do not see skills added since
startup; call `reload_skills` once after a skill is installed or removed on disk,
then list/load it. A failed rescan (e.g. a malformed `SKILL.md`) is reported and
leaves the previous catalog in place.
