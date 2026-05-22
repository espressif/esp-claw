# Edge Agent Guide

## How It Works

The main entry point is `application/edge_agent/main/main.c`.

After the device boots, the overall flow is:

1. Initialize NVS and load device settings
2. Mount FATFS at `/fatfs`
3. Initialize Wi-Fi and the local HTTP configuration service
4. Enter `app_claw_start()`
5. Initialize the event router, memory, skills, and capabilities
6. Initialize and start `claw_core`
7. Start the CLI and begin handling requests and events

The current runtime depends on the following local directories:

- `/fatfs/sessions`: session history
- `/fatfs/memory/MEMORY.md`: long-term memory
- `/fatfs/skills`: skill documents and manifest
- `/fatfs/scripts`: Lua scripts
- `/fatfs/router_rules/router_rules.json`: automation rules
- `/fatfs/inbox`: message attachment storage

## FATFS Image Layout

The base FATFS image content lives in:

```text
application/edge_agent/fatfs_image/
```

Files and directories under this path are copied into the build-time image staging directory:

```text
application/edge_agent/build/fatfs_image/
```

Each board can also provide optional board-specific FATFS content under its own board directory:

```text
application/edge_agent/boards/<vendor>/<board>/fatfs_image/
```

During the build, `application/edge_agent/CMakeLists.txt` first copies the base `fatfs_image/` directory, then copies the selected board's `fatfs_image/` directory if it exists. The selected board path comes from the generated `components/gen_bmgr_codes/CMakeLists.txt`, which is produced by `idf.py gen-bmgr-config`.

If a board-specific file has the same relative path as a base file, the board-specific file overwrites the base file in `build/fatfs_image/`. This lets a board replace defaults such as skills, scripts, router rules, static assets, or memory files without changing the shared base image.

The current app integrates the following capabilities:

- `cap_im_qq`
- `cap_im_tg`
- `cap_files`
- `cap_lua`
- `cap_mcp_client`
- `cap_mcp_server`
- `cap_skill_mgr`
- `cap_time`
- `cap_llm_inspect`
- `cap_web_search`

## Quick Start

### Prerequisites

- ESP-IDF is installed and exported
- `ESP-IDF v5.5.4` is recommended

```bash
. <your-esp-idf-path>/export.sh
```

### Configuration

To make `esp-board-manager` easier to use, first install the helper package with `pip install esp-bmgr-assist`. You only need to do this once in a given ESP-IDF environment.

1. Generate board support files:

```bash
cd application/edge_agent
idf.py gen-bmgr-config -c ./boards -b esp32_S3_DevKitC_1
```

> `idf.py gen-bmgr-config -c ./boards -b <board_name>` generates the configuration for the specified board. Available board names can be found in the `boards` directory.

2. Configure Wi-Fi, LLM, IM, search engine, and related parameters:

The key demo settings include:

- Wi-Fi SSID / Password
- LLM API Key / Provider / Model
- QQ App ID / App Secret
- Telegram Bot Token
- Brave / Tavily Search Key
- Timezone

Key Notes:

- IM bot token: available from Telegram [@BotFather](https://t.me/BotFather) or [QQ Bot](https://q.qq.com/qqbot/openclaw/login.html)
- LLM API key: available from [Anthropic Console](https://console.anthropic.com), [OpenAI Platform](https://platform.openai.com), or [Alibaba Cloud Bailian](https://bailian.console.aliyun.com/#/api-key)

You can adjust compile-time default values through `menuconfig`:

```bash
idf.py menuconfig
```

3. Build and flash:

```bash
idf.py build
idf.py flash monitor
```
