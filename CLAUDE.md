# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Is

ESP-Claw is an AI agent framework for ESP32-series IoT devices, written in C on top of ESP-IDF. It runs an LLM-driven agent loop locally on the chip: incoming messages from IM platforms (Telegram, QQ, WeChat, Feishu) or hardware events are routed through a rule engine, dispatched to the agent, which calls capabilities (tools) in an iterative loop until the task is complete.

## Build and Flash (application/basic_demo)

Prerequisites: ESP-IDF v5.5.4 exported (`idf.py` must be on PATH). Also install the board manager helper once:

```bash
pip install esp-bmgr-assist
```

From `application/basic_demo/`:

```bash
# Generate board support files (run once per board)
idf.py gen-bmgr-config -c ./boards -b esp32_S3_DevKitC_1

# Configure LLM/Wi-Fi/IM credentials
idf.py menuconfig

# Build
idf.py build

# Flash and monitor
idf.py flash monitor
```

Available board names are subdirectories of `application/basic_demo/boards/`.

## Docs Site (docs/)

Built with Astro/Starlight, managed with pnpm:

```bash
cd docs
pnpm install
pnpm dev      # local dev server
pnpm build    # production build
```

## Code Style

Pre-commit hooks enforce formatting. Install once:

```bash
pip install pre-commit
pre-commit install
```

C code is formatted with AStyle (rules in `.gitlab/ci/astyle-rules.yml`). Commit messages must follow Conventional Commits with `--subject-min-length=15`.

## Architecture

### Core data flow

```
IM / hardware event
    → claw_event_t
    → claw_event_router  (rule-based dispatch, loaded from /fatfs/router_rules/router_rules.json)
    → claw_core          (LLM agent loop: iterative tool-calling until done)
    → claw_cap           (capability dispatcher — routes tool calls to registered caps)
    → outbound binding   (e.g. tg_send_message → Telegram)
```

### Layer overview

**`components/claw_modules/`** — framework core, no app-specific logic:

- `claw_core` — LLM agent loop. Accepts `claw_core_request_t`, builds an LLM prompt from context providers, runs iterative tool-call rounds (up to `max_tool_iterations`), emits a response. Backends: OpenAI-compatible and Anthropic (`src/llm/backends/`).
- `claw_cap` — capability registry. Capabilities (`claw_cap_descriptor_t`) are registered in groups, flagged as `CLAW_CAP_FLAG_CALLABLE_BY_LLM` when they should be exposed as tools. The LLM sees only the groups listed in `llm_visible_groups` (plus any session-scoped additions).
- `claw_event_router` — rule engine. Rules (JSON at `/fatfs/router_rules/router_rules.json`) match incoming `claw_event_t` fields and dispatch to actions: call a cap, submit to agent, run a Lua script, send a message, or drop.
- `claw_memory` — structured long-term memory stored on FATFS (`/fatfs/memory/`). Provides context providers injected into each LLM request: profile, long-term facts, session history.
- `claw_skill` — skill system. Skills are Markdown documents in `/fatfs/skills/` that the agent activates on demand. Activating a skill unlocks additional capability groups for that session.

**`components/claw_capabilities/`** — pluggable capability groups:

Each capability follows the same pattern: a `cap_<name>.c` that registers a `claw_cap_group_t` via `cap_<name>_register_group()`, optional `skills/` docs, optional `cmd_cap_<name>.c` for console commands.

- IM caps (`cap_im_tg`, `cap_im_qq`, `cap_im_feishu`, `cap_im_wechat`): long-poll inbound messages, publish as events, expose send/reply as callable caps.
- `cap_lua` — runs Lua scripts on-device. Supports sync and async execution. Lua modules in `components/lua_modules/` expose hardware (GPIO, I2C, display, camera, audio, etc.) to Lua.
- `cap_scheduler` — cron-like scheduler; persists to `/fatfs/scheduler/schedules.json`, fires events on schedule.
- `cap_mcp_client` / `cap_mcp_server` — Model Context Protocol over network, allowing the device to act as both MCP client and server.
- `cap_skill_mgr` — exposes `activate_skill`/`deactivate_skill` as LLM-callable tools.
- `cap_router_mgr` — lets the agent manage routing rules at runtime.
- `cap_files`, `cap_time`, `cap_web_search`, `cap_llm_inspect`, `cap_system`, `cap_session_mgr`.

**`application/basic_demo/`** — the reference application:

- `main/main.c` — boots hardware, mounts FATFS, starts Wi-Fi, then calls `app_claw_start()`.
- `main/app_claw.c` — wires together all modules: initializes paths, event router, memory, skills, all capability groups, and `claw_core`. This is the integration point for the whole framework.
- `boards/` — per-board hardware setup (`setup_device.c`, sdkconfig, YAML declarations for board manager).
- `fatfs_image/` — initial filesystem content baked into the flash image.
- `tools/cmake/skills_sync.cmake` — CMake rule that copies skill docs from components into the FATFS image at build time.

### Context provider pattern

`claw_core` assembles the LLM prompt from an ordered list of `claw_core_context_provider_t` registered via `claw_core_add_context_provider()`. Each provider contributes either a system-prompt snippet, message history, or tool definitions. The capability catalog, skill docs, session history, and memory are all injected this way — nothing is hardcoded into the agent loop itself.

### FATFS runtime layout

```
/fatfs/
  memory/        long-term memory (MEMORY.md, records, index)
  sessions/      per-session conversation history
  skills/        skill Markdown files + skills_list.json manifest
  scripts/       Lua scripts (builtin/ synced from components at build time)
  router_rules/  routing rules JSON
  scheduler/     cron schedule JSON
  inbox/         inbound IM attachments
```

### LLM backends

Configured via `llm_profile` / `llm_backend_type` in settings. Supported profiles: `openai`, `anthropic`, `qwen`, `qwen_compatible`, or custom endpoint. The Anthropic backend uses its own request/response format; all others use OpenAI-compatible streaming.
