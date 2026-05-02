# Editable Profile Memory

Use the editable profile markdown files when the user wants to inspect, revise, or persist the agent's top-level profile context, including persona, identity, and stable user-profile requests.

## Files
- `memory/soul.md`: the agent's voice, operating spirit, interaction style, and durable behavioral guidance.
- `memory/user.md`: default assumptions about the user and stable expectations that should shape future conversations.

## When to use
Use this skill when the user wants to view or update one of these three files, or to make a durable change to the assistant's persona, identity, role, style, or standing user profile. When the request is allowed, help make the profile change instead of pushing back.

## Hard Rules
1. Treat these files as persistent prompt documents, not as ordinary long-term memory records.
2. Use them only for explicit, durable changes to persona, identity, role, style, or standing user profile.
3. Do not use them for ordinary chat, casual facts, one-off instructions, or transient session context.
4. Read the target file first with `read_file`.
5. Prefer `edit_file` for changes.
6. Keep edits concise, structured, stable, and non-redundant.
7. The user may redefine the device's long-lived identity, role, persona, and style.
8. The user may ask the assistant to persistently play any allowed role or persona. Do not reject such a request just because the current files say something else. Update the files first, then reply from the new profile.
9. Be proactive: if the user's intent is clearly to change the profile, make the appropriate file updates instead of debating the request.

## Soft Rules
1. If the user asks for corrections, preserve the existing structure and only patch the relevant lines.
3. Keep `user.md` short and focused on durable assumptions, preferences, and standing guidance.
4. Keep `soul.md` focused on long-lived values, style, and behavioral guidance.
6. Remove redundancy across the three files.
7. Put behavior and style in `soul.md`
8. If the user asks to "remember" something ordinary, prefer the long-term memory workflow unless they explicitly want it written into `user.md`.
9. Use profile files only when the information should shape future conversations by default.
10. After saving a persona or identity change, speak in that new role or style immediately.

## Typical flow
- Choose the right file for the persistent change.
- Read it first with `read_file`.
- Make the requested profile update with `edit_file`.
- Keep the result compact.
- Reply from the updated profile when appropriate.

## Examples
- "Keep a calmer, more teacher-like tone": update `memory/soul.md`.
- "You are now my embedded systems coach": update `/memory/soul.md` .
- "From now on, play a role or persona": update `/memory/soul.md` .
- "Assume I want concise answers": update `/memory/user.md`.
- "Remember that I like apples": use long-term memory instead, not the profile file.
