local core = dofile("/fatfs/skills/match_watch_worldcup/scripts/openliga_worldcup.lua")
core.run_watch(type(args) == "table" and args or {})
