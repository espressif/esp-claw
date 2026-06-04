# Documentation Guide

Use this guide when a task changes user-visible behavior, public APIs, supported hardware, built-in Capabilities, Lua modules, setup flows, or examples in the docs site.

Keep this file practical. Prefer short rules, exact paths, and runnable checks over generic writing advice.

## Scope

- All end-user documentation lives under `docs/`.
- English and Simplified Chinese docs must stay aligned in structure and meaning.
- Treat `docs/src/content/docs/zh-cn/` as the source layout to inspect first, then mirror equivalent updates in `docs/src/content/docs/en/`.

## Documentation Trigger Rules

- After changing core behavior, search the docs for affected concepts and update existing pages so they match the current code.
- "Core behavior" includes boot/runtime flow, router behavior, memory behavior, session behavior, capability behavior, board support, CLI/setup steps, and Lua-facing APIs.
- Do not leave stale behavior descriptions in place just because the code change was small.
- If no matching docs exist for a code change, only add new docs when the feature is important for typical users.

## What To Search

Before editing docs, search both code and docs to find the current source of truth.

- Search docs with `rg` from repo root, for example:
  - `rg -n "router|memory|session|<feature-name>" docs/src/content/docs`
  - `rg -n "lua_module_|lua_driver_|<capability-name>" docs/src/content/docs`
- If there are existing docs on a similar topic, review their structure first and reuse the same section shape when it fits.
- Prefer matching established patterns for headings, intro depth, tables, admonitions, examples, and source-reference placement.
- Search implementation when wording is unclear:
  - `rg -n "<feature-name>" components application/edge_agent`
- Prefer updating an existing page over creating a new one if the topic already has a natural home.

## Capability Documentation Policy

- Capabilities live under `components/claw_capabilities/`.
- When a core Capability changes and it is already documented, update the existing doc page.
- When adding a new Capability, do not create a dedicated doc page by default.
- Create a standalone Capability page only for representative or high-value Capabilities that help users understand a broader pattern, workflow, or integration style.
- For less central Capabilities, prefer mentioning them in overview/reference pages instead of creating one page per Capability.

## Lua Module Documentation Policy

- If a task adds or removes any `lua_module_*` or `lua_driver_*` component, update:
  - `docs/src/content/docs/en/reference-cap/lua-modules.mdx`
  - `docs/src/content/docs/zh-cn/reference-cap/lua-modules.mdx`
- Do not create a new standalone docs page for most `lua_module_*` or `lua_driver_*` additions unless the user explicitly asks for one.
- Keep the built-in module lists complete and synchronized with the codebase.

## Multilingual Alignment Rules

- Every doc change in one language must be mirrored in the other supported languages.
- File structure, headings, list shape, tables, admonitions, and code block placement must stay aligned across languages.
- Line alignment is mandatory:
  - If one language has a blank line, the corresponding line in the other language must also be blank.
  - If one language has text on a line, the corresponding line in the other language must also have text.
- When editing paired files, make small mirrored edits instead of rewriting only one language, because large asymmetric rewrites usually break line alignment.

## Writing Style

- Write for users, not for maintainers debugging internals.
- Keep claims tied to observable behavior in the current codebase.
- Prefer explicit paths, component names, and commands.
- When mentioning source code in docs, prefer using `docs/src/components/LinkToSource.astro` instead of pasting raw repository URLs.
- Mark conditional behavior clearly when it depends on Kconfig, board support, hardware presence, or build configuration.
- Avoid documenting planned features, speculative behavior, or APIs that are not implemented.
- Keep examples short and runnable where possible.

## Recommended Workflow

1. Identify the code change and its user-visible impact.
2. Search `docs/` for existing references to the affected feature.
3. Update the best existing page in `en/`.
4. Mirror the same change in `zh-cn/` while preserving line-by-line alignment.
5. If the change adds a `lua_module_*` or `lua_driver_*`, update `reference-cap/lua-modules.mdx` in both languages.
6. If the change adds a Capability, decide whether it is representative enough to deserve a dedicated page; otherwise update overview/reference docs only.
7. Run the required validation commands.
8. If build or alignment checks fail, fix the docs instead of weakening the checks.

## Validation

Run these commands after documentation edits:

```bash
cd docs
pnpm run check:doc-lines
pnpm run build
```

`check:doc-lines` is required whenever multilingual docs change. `build` is required to catch MDX, frontmatter, link, and component usage problems.
