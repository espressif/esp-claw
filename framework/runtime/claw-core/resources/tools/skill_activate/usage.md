Activate one skill by `skill_id` from `skill_list`. The tool returns the
processed skill document immediately, wrapped as `<skill_content name="...">`.
Activation is a one-shot document read; it does not create persistent loaded
state. For a skill just added or edited on disk, run `skill_reload` first.
