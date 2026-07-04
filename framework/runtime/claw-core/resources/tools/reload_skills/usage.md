Re-scan the skills directories from disk and refresh the catalog. `list_skill`
and `activate_skill` read a cached snapshot for speed and do not see skills
added since startup; call `reload_skills` once after a skill is installed,
edited, or removed on disk. A failed rescan is reported and leaves the previous
catalog in place.
