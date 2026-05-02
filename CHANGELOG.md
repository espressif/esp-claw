# ChangeLog

## 2026-04-30

### Feature:

* Added `esp_SensairShuttle` board support for `edge_agent`, including board metadata, peripheral definitions, default board config, and device setup wiring.

* Added shared `app_claw` integration for the new Lua environmental sensor and magnetometer modules, including Kconfig, component dependencies, and Lua module registration.

* Added the `lua_module_environmental_sensor` module with Lua-facing sensor APIs and a `basic_environmental_sensor.lua` example script.

* Added the `lua_module_magnetometer` module with bundled `bmm350` driver sources, Lua bindings, example scripts, and skill metadata.

* Renamed the `lua_module_bq27220` module to the more generic `lua_module_fuel_gauge`, and refreshed its Lua examples, helper library, and skill docs.

* Added `esp32_p4_eye` board support for `edge_agent`, including board device/peripheral definitions, board defaults, CI build coverage, and early board bring-up for camera, SD power, and LCD panel initialization.

* Added shared `app_claw` support for `lua_module_knob`, including Kconfig and dependency wiring, Lua module registration, a basic rotary encoder demo script, and the related skill metadata.

* Make max tokens configurable

* Sent Feishu `feishu_send_message` responses as Markdown-capable interactive cards with plain-text fallback. Added Feishu inbound rich text `post` message flattening to Markdown text, including styled text, links, mentions, code blocks, horizontal rules, and image/file placeholders.

* Preserved Feishu inbound file names in saved attachment paths while keeping the message hash prefix for uniqueness.

* Added Feishu rich text `post` embedded attachment handling so inline images and media/file elements are queued through the existing attachment save flow.

* Added a guided Edge Agent setup wizard for first-run configuration, covering LLM provider presets, search provider keys, and IM platform setup with built-in WeChat QR login flow.

* Enhanced the Edge Agent web chat experience with local chat session persistence, file upload support, richer status and restart feedback, and updated configuration editing UI.

* Refactored the docs online flashing workflow with a redesigned multi-step flash page, refreshed localized copy, and updated firmware metadata generation for the new tool flow.

### Change:

* Removed the deprecated `application/basic_demo` app and its CI/build rules.

* Simplified the docs flash tool implementation by consolidating firmware selection and flashing logic into the main page flow and removing the old helper modules.

### Fix:

* Fixed the `wifi --apply` flow so updated STA settings are applied immediately, with more reliable AP fallback and reconnect handling in `wifi_manager`.



## 2026-04-29

### Feature:

* Updated the emote layout asset to change the GFX label scroll speed.

* Refactored the Web Config interface:
  * Introduced support for fine-grained configuration controls 
  * Added a basic online chat module.

* Added a boot-complete startup trigger event in the shared app and Basic Demo startup flow, plus a disabled-by-default router rule example that runs `hello.lua` on boot.

* Added support for the following third-party development boards:
  * `m5stack_cores3` (PR #5, contributed by @imliubo)
  * `m5stack_sticks3` (PR #6, contributed by @imliubo)
  * `dfrobot_k10` (PR #28, contributed by @wxzed)
  * `lilygo_t_display_s3` (PR #14, contributed by @terry-cook)

* Added shared `app_claw` support for the pure-Lua SSD1306 module, including Kconfig and dependency wiring so SSD1306 scripts and skills can be pulled into app builds that enable Lua support.

* Enhanced CI board builds to support optional brand-specific board paths, emit `board_brand` in merged binary metadata, and append ESP32-P4 revision suffixes to generated merged binary artifacts while recording `rev` in the output JSON.

* Added shared `app_claw` support for `lua_module_lcd`, including Kconfig, component dependency, and Lua module registration wiring so LCD scripts can be enabled from shared app builds with Lua support.

* Increased the Claw capability tool result buffer to 32 KB so larger tool responses can be returned. Updated `cap_files` file reads to reject oversized files before reading and return an explicit error when the file exceeds the max read limit.
  
* Increased Claw memory session message buffers to 4096 chars.
  
* Raised the Edge Agent tool iteration limit to 32 for longer multi-step interactions.

* Updated Claw memory Session History collection to size its JSON buffer from retained session records instead of worst-case configuration limits, reducing unnecessary heap usage for short histories.

* Added the `edge_agent` demo application with Wi-Fi setup, HTTP management UI, FATFS image content, Lua script examples, skill assets, router rules, scheduler rules, and partition defaults.

* Added shared application components for Claw capability wiring, CLI support, Lua module registration, captive DNS, display arbitration, emote rendering, settings storage, and Wi-Fi management.

* Added Edge Agent web APIs and frontend pages for configuration, status, file access, capabilities, Lua modules, and WeChat integration.

* Added I2C Lua module: Introduced support for hardware I2C bus initialization, device scanning, and standard register read/write operations.

* Added UART Lua module: Introduced UART port open/close, polling-based read/write and line reads, with a basic demo script.

* Added ADC Lua module: Introduced ADC channel creation, reading, and closing operations, with a basic demo script.

* Added file copy and move operations to `cap_files`, including automatic parent directory creation and rename fallback when direct moves are not available.

* Reworked `cap_time` to use SNTP-based synchronization, added on-demand current time retrieval, and injected a time context provider for relative date reasoning in `claw_core`.

### Change:

* Migrated selected Basic Demo updates into Edge Agent, including app configuration handling, HTTP UI updates, Lua module wiring, and default SDK configuration changes.

* Moved `dfrobot_k10` and other shipped board definitions into vendor-specific subdirectories, and relocated the LilyGO T-Display-S3 board assets from `basic_demo` to `edge_agent`.

### Fix:

* Persisted failed `claw_core` agent turns to session history so follow-up turns can see prior failure traces.
* Cleared context outputs before collection across Lua jobs, tool catalogs, memory providers, and skill prompts so failed collection paths do not leak stale content.

* Fixed LilyGO T-Display-S3 LCD startup behavior.

## 2026-04-28

### Fix:

* Reduced `httpd` task stack pressure in Edge Agent by moving large configuration and WeChat login status structures from stack allocation to heap-backed buffers across the config and WeChat HTTP handlers.

## 2026-04-27

### Feature:

* Added microsecond delay support to `lua_module_delay`.

* Extended `lua_module_mcpwm` with dual-channel output support, updated the MCPWM demo script, and refreshed the related skill guide.

## 2026-04-21

### Refactor:

* Simplified skill activation and deactivation inputs from single `skill_id` to batch-oriented `skill_ids`, and added `all=true` support for clearing active skills.
* Updated skill manager results to echo requested skills and keep session-visible capability groups in sync after batch operations.
* Updated the display skill guidance.

### Feature:

* Synced builtin Lua scripts from component `lua_scripts` directories into `basic_demo`, replacing the old scattered `fatfs_image/scripts/builtin` layout.
* Added `sync_component_lua_scripts.py` and extended CMake sync flow so builtin scripts and generated manifests can be maintained automatically.
* Added router example scripts for LLM analyze and message/file publishing, then removed the send-message and send-file examples to narrow the shipped set.

### Fix:

* Preserved reasoning content in tool-call history across `claw_core` and OpenAI-compatible LLM backend flows.
* Updated `cap_lua` skill docs to prefer reusing existing scripts before creating new ones.
* Refined capability skill documentation and tightened `cap_router_mgr` guidance to reduce ambiguous tool usage.

## 2026-04-20

### Refactor:

* Simplified `cap_lua` argument handling so `lua_run_script` and `lua_run_script_async` only forward explicit `args` payloads instead of auto-merging agent session context.
* Simplified `event_publisher.publish_message` to require a table input with explicit `source_cap`, `channel`, `chat_id`, and `text` fields.
* Removed runtime `taskYIELD` behavior from the Lua timeout hook and relaxed the hook cadence to run every 1000 instructions.
* Normalized JSON number handling in Lua runtime to always push numbers as Lua numbers instead of preserving integer form.
