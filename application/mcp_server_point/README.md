# mcp_server_point (ESP-Claw)

mcp_server_point is a **lean ESP-IDF application** in this repo: Wi-Fi configuration from **`app_config`** defaults / NVS, **`storage` FAT** for Claw data, and **`app_claw`** configured for a **small capability set** centered on **`cap_lua`**, plus **`cap_mcp_server`** with MCP Lua tools started from **`main.c`**.

Full-stack reference for IM, files, scheduler, event router, and so on lives in **`application/edge_agent`**. mcp_server_point is the trimmed variant.

---

## What runs on boot

Entry point: `main/main.c`. Typical sequence:

1. **NVS** init, **`app_config`** load (Wi-Fi from NVS namespace `app`; Claw `enabled_cap_groups` fixed to **`cap_lua`**).
2. **`/fatfs`** mount on partition `storage` (wear-leveled FAT).
3. **`wifi_manager`** starts from the SSID/password loaded by **`app_config`**.
4. Optional **STA connect**; on failure or empty SSID the Wi-Fi manager may keep AP fallback active. There is **no** HTTP provisioning portal.
5. **`claw_paths_set`** for `/fatfs` (DATA and SYSTEM roots), then **`app_claw_start`** — with defaults this only runs **`app_capabilities_init`** for **`cap_lua`** (`CONFIG_APP_CLAW_CAP_CORE=n`, no session/agent/memory/skill/IM/event-router).
6. **`cap_mcp_server`** (outside `app_claw` cap groups): `cap_mcp_server_init` → **`cap_mcp_lua_tools_init`** → **`cap_mcp_server_start`**.

There is **no** App Claw serial CLI (`CONFIG_APP_CLAW_ENABLE_CLI=n`), no `register_wifi_command()`, and no HTTP provisioning UI. Set default Wi-Fi under **App Config** in menuconfig, or use persisted NVS settings.

---

## Repository layout (this app)

| Path | Role |
|------|------|
| `main/` | `app_main`, `idf_component.yml` (app_claw, wifi_manager, cap_lua, cap_mcp_server, board manager, …) |
| `components/app_config/` | NVS-backed Wi-Fi; sets `app_claw_config_t.enabled_cap_groups` to `"cap_lua"` |
| `components/mcp_server_point_tools/` | MCP tools (`lua.run_script`, async jobs, …) registered into `cap_mcp_server` |
| `components/gen_bmgr_codes/` | Board-manager glue; **board choice must match** the `boards/...` tree referenced here |
| `boards/` | `esp-board-manager` board definitions (`idf.py gen-bmgr-config`) |
| `fatfs_image/` | Source tree for the **`storage`** SPI-FAT image (build-synced builtin Lua modules) |
| `tools/` | Project CMake helpers (IDF patch, partition defaults) |

Root **`CMakeLists.txt`** builds project **`mcp_server_point`**, creates the **`storage`** image from `fatfs_image/`, and syncs **builtin Lua** into `fatfs_image/scripts/builtin` before packing.

---

## FAT layout (`/fatfs`)

mcp_server_point seeds Lua scripts on FAT:

- `scripts/` — user scripts and preloaded examples
- `scripts/builtin/` — synced at build from `lua_module_builder`

Initial sources live under **`fatfs_image/`** in git; generated sync output is listed in **`application/mcp_server_point/.gitignore`**.

mcp_server_point does not use **`claw_event_router`** or `router_rules.json`. MCP clients call **`lua.run_script`**, **`lua.run_script_async`**, **`lua.list_jobs`**, **`lua.get_job`**, **`lua.stop_job`**, and **`lua.stop_all_jobs`**; scripts must already exist under `scripts/` (for example under `fatfs_image/`).

---

## Capability profile (defaults)

`application/mcp_server_point/sdkconfig.defaults` sets app-level defaults, while
board-level defaults come from `boards/<vendor>/<board>/sdkconfig.defaults.board`
after running `idf.py gen-bmgr-config`.

| Area | Default |
|------|---------|
| Flash/Partition | Defaults to `partitions_16MB.csv`; flash size is taken from board defaults (for `esp_Ditto`: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`) |
| App Claw CLI | **off** (`CONFIG_APP_CLAW_ENABLE_CLI=n`) |
| Enabled caps | **`CONFIG_APP_CLAW_CAP_LUA=y`**, **`CONFIG_APP_CLAW_CAP_MCP_SERVER=y`** |
| Off caps | memory, event router, claw_core (`CORE=n`), agent_mgr, IM, scheduler, files, skill_mgr, session_mgr, router_mgr, time, system, web_search, llm_inspect, mcp_client |
| Lua | `CONFIG_APP_CLAW_LUA_MODULE_BLE_HID=n` (no BT stack); `CONFIG_APP_CLAW_LUA_MODULE_EVENT_PUBLISHER=n` (no event router) |
| Network/RTOS | `CONFIG_FREERTOS_HZ=1000`, `CONFIG_LWIP_MAX_SOCKETS=20`, `CONFIG_LWIP_LOCAL_HOSTNAME="esp-claw"` |

For `boards/espressif/esp_Ditto`, board defaults additionally enable PSRAM (OCT, 80M),
USB Serial JTAG console, BF3901 camera config, and IMU/magnetometer/fuel-gauge Lua modules.

With **`CONFIG_APP_CLAW_CAP_MEMORY=n`**, do not set **`APP_CLAW_MEMORY_MODE_*`** in menuconfig; memory mode only applies when memory is enabled.

**`app_config`** restricts Claw registration to the **`cap_lua`** group. **`cap_mcp_server`** is initialized in **`main.c`** after `app_claw_start`, not via `enabled_cap_groups`.

MCP Lua tool implementations live in **`mcp_server_point_tools`** (`cap_mcp_lua.c`).

Use **`idf.py menuconfig`** to change **`APP_CLAW_CAP_*`**, Lua driver/module options, and board-specific settings.

---

## Prerequisites

- **ESP-IDF** installed and exported (developed against **v5.5.x**, e.g. 5.5.4).
- Optional: `pip install esp-bmgr-assist` for board-manager CLI ergonomics.

```bash
. <path-to-esp-idf>/export.sh
```

---

## Board support files

Board definitions live in **`./boards`** (not under `edge_agent`).

```bash
cd application/mcp_server_point
idf.py gen-bmgr-config -c ./boards -b <board_id>
```

Current generated board defaults in this app target `espressif/esp_Ditto`
(`components/gen_bmgr_codes/board_manager.defaults`).

Examples of available board IDs in this app: `espressif/esp_Ditto`.

**Note:** `components/gen_bmgr_codes/CMakeLists.txt` points at a **specific** board directory for codegen. After switching boards, regenerate board config **and** align that component’s paths so build and hardware match.

---

## Build and flash

From `application/mcp_server_point`:

```bash
idf.py build
idf.py -p PORT flash monitor
```

Firmware artifact: **`build/mcp_server_point.bin`**.

---

## Flash image and partitions

- **`fatfs_create_spiflash_image`** builds **`storage.bin`** from `fatfs_image/` and flashes it with the app when **`FLASH_IN_PROJECT`** is enabled.
- Current default partition table is **`partitions_16MB.csv`**.
- Partition CSV is auto-selected from board flash size by `tools/cmake/flash_partition_defaults.cmake`.
  For `esp_Ditto`, this resolves to `partitions_16MB.csv`.

---

## Further reading

- **`application/edge_agent/README.md`** — full agent (CLI, more caps, event router).
- Repo **`docs/`** — architecture and capability naming.
