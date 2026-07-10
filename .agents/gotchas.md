# Common Gotchas

## Capability and Lua selection are config-driven

Enabled capability groups, LLM-visible groups, and enabled Lua modules come from app configuration. Empty selections usually mean "use defaults" or "enable available modules"; unknown tokens are ignored with warnings.

When adding a new capability group or Lua module, update the app registration table and the relevant Kconfig/default configuration path.

## Lua driver READMEs are for agents

`components/lua_modules/lua_driver_xxx/README.md` files are agent-facing documentation. They explain how an agent should use the Lua driver and are meant to be read as operating instructions, not as user marketing docs or full developer manuals.

Keep these READMEs concise, capability-oriented, and accurate for runtime Lua usage.
