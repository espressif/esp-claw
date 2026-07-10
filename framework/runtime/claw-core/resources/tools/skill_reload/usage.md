Re-scan the skills directories from disk and refresh the catalog. `skill_list`
and `skill_activate` read a cached snapshot for speed and do not see skills
added since startup; call `skill_reload` once after a skill is installed,
edited, or removed on disk. A failed rescan is reported and leaves the previous
catalog in place.
