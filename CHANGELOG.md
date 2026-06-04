# ChangeLog

## 2026-05-19

### Feature:

* Added Markdown preview support in Web Chat. (Related to https://github.com/espressif/esp-claw/issues/36)

### Fixes:

* Web Chat:
  * Append messages immediately instead of waiting for send success. (Related to https://github.com/espressif/esp-claw/issues/36)
  * Use visual indicators for send status and allow retries.
  * Preserve Web Chat history when switching between tabs.
* Board: Adjusted partition table size to accommodate larger bootloaders on some development boards.

### Change:

* **Breaking change**: Renamed the Claw Core write-side session persistence API to context persistence for clearer semantics. Applications should migrate `persist_session` to `persist_context`, `persist_session_user_ctx` to `persist_context_user_ctx`, `claw_core_persist_session_fn` to `claw_core_persist_context_fn`, `claw_session_persist_batch_t` to `claw_core_context_persist_batch_t`, `claw_session_record_t` to `claw_core_context_record_t`, `claw_session_record_type_t` to `claw_core_context_record_type_t`, `CLAW_SESSION_RECORD_*` to `CLAW_CORE_CONTEXT_RECORD_*`, and `claw_memory_persist_session_callback` to `claw_memory_persist_context_callback`. `session_id` and `claw_memory_session_history_provider` are unchanged.

## 2026-05-18

### Feature:

* Added typed raw session history persistence for Claw Core and Claw Memory, preserving the full Agent turn flow: user input, raw assistant tool-call messages, tool results, final assistant messages, and failure notes.
* Preserved raw assistant messages from OpenAI-compatible and Anthropic-compatible backends so tool-call history can be replayed without reconstructing backend-specific message shapes.
* Added indexed append-only session history storage using JSON data records and paired `.idx` entries with offset, length, record type, and backend format.
* Added size-based session compaction and oversized-session blocking. Compaction keeps user and final assistant records, recent tool-turn records, and unfinished current-turn records; blocked sessions return `/new` guidance instead of growing history further.
* Added a Claw Core request gate callback and request-start-only context provider flag for blocked-session rejection and once-per-request history collection.
* Degraded final assistant records to plain text across incompatible backend formats, while skipping unsafe backend-specific tool records.

### Change:

* Replaced `append_session_turn` with the `persist_session` typed record batch callback. Applications should migrate to `claw_memory_persist_session_callback` or provide an equivalent `claw_core_persist_session_fn`.
* Removed `max_session_messages` from `claw_memory_config_t`; session retention now uses size limits and compaction.
* Removed the Claw Memory `mbedtls` private dependency previously used for encoded session headers.

### Fix:

* Preserved Anthropic `redacted_thinking` blocks and propagated tool-result error state when converting tool messages for Anthropic-compatible requests.

## 2026-05-11

### Feature:

* Added Lua access to the SCI sensor for DFRobot-K10. (@rockets-cn, https://github.com/espressif/esp-claw/pull/32)
* Added configurable AP name and password settings for Wi-Fi. (@Karasukaigan, https://github.com/espressif/esp-claw/pull/63) (Related to https://github.com/espressif/esp-claw/issues/41)
* Used a more stable system prompt to improve cache hit rates. (ae_group/esp-claw!133)

### Board:

* Added movecall_cuican_esp32s3, movecall_moji_esp32s3, movecall_moji2_esp32c5. (@MoveCall, https://github.com/espressif/esp-claw/pull/50)
* Added Waveshare ESP32-P4 NANO. (@yuzheyi, https://github.com/espressif/esp-claw/pull/59)
* Added NologoTech Xingzhi-395. (@vaemc, https://github.com/espressif/esp-claw/pull/55)

### Fix:

* Improved patch version compatibility across different IDF versions. (ae_group/esp-claw!134)
* Fixed UTF-8 sanitization in the LLM request body to prevent HTTP 400 errors. (@yuzheyi, https://github.com/espressif/esp-claw/pull/58)
* Preserved thinking blocks and merged consecutive tool results to fix an Anthropic API compliance issue. (@zz6zz666, https://github.com/espressif/esp-claw/pull/40)
* Fixed missing `Console Output` declarations in some development builds. (ae_group/esp-claw!132)

## 2026-05-09

### Fix:

* LLM HTTP transport (`claw_llm_http_post_json`): copy and sanitize JSON request bodies so invalid UTF-8 sequences are replaced before POST, avoiding stack/client issues on malformed input. (https://github.com/espressif/esp-claw/pull/58, Thanks @yuzheyi.)

### Feature:

* Stablized the system prompt:
  * `activate_skill` now accepts one `skill_id` per call and returns the full Skill markdown document in a `<skill_content>` tool result.
  * Removed automatic active Skill document prompt injection and the `deactivate_skill` flow.
  * Removed time context and part of session context from system prompt to keep it stable.
  * Removed lua async job infomation from system prompt.

## 2026-05-08

### Refactor:

* **Breaking change**: Removed the LLM Profile concept. You may need to update your LLM configuration accordingly.
  * A forward compatibility transition has been added for now and will be removed in a future release.

## 2026-05-07

### Tools:

* Online Flashing Tool: Supported flashing firmware with different console outputs

### Feature:

* Added the `take_picture` Lua module skill for camera-enabled boards, including a guarded JPEG capture script with filename/directory validation and saved-frame reporting.

* Added `system.heap` APIs to `lua_module_system` for heap capability constants, heap statistics, task stack high-water marks, and current-task stack inspection.

* Added Lua test scripts for web search and IM send capability calls.

* Added Skill Creator guidance and reference material for authoring Lua-backed Skills, including script creation workflow, file tool usage, and runnable script conventions.

### Fix:

* Added WebSocket heartbeat support to Web Chat for improved reliability. (https://github.com/espressif/esp-claw/issues/36)

* Disabled default Lua and system capability registration in shared app capability wiring so applications only enable them explicitly.

### Change:

* Merged Feishu, QQ, Telegram, WeChat, and IM attachment sources, Skills, and docs into the unified `cap_im_platform` component while keeping existing per-platform runtime group IDs and tool names.

* Removed the standalone `lua_module_esp_heap` module and folded its heap introspection APIs into `lua_module_system` as `system.heap`.

* Updated `cap_lua` script management to rely on file tools (`list_dir`, `read_file`, and `write_file`) for discovering and editing scripts instead of a dedicated Lua script listing tool.

### Refactor:

* Renamed six hardware-peripheral Lua modules from `lua_module_*` to `lua_driver_*` (adc, gpio, i2c, mcpwm, touch, uart) to distinguish low-level drivers from higher-level modules. Updated directory names, source filenames, all internal symbols, Kconfig options (`APP_CLAW_LUA_MODULE_*` → `APP_CLAW_LUA_DRIVER_*`), CMake dependencies, `idf_component.yml` entries, and documentation references.

## 2026-05-06

### Feature:

* Merged DHT-family single-wire sensor support into `lua_module_environmental_sensor`, allowing the same module to drive both BME690 (I2C) and DHT11/DHT22/AM2301/AM2302/AM2321/SI7021 sensors via separate Kconfig backends.

* Added MPU6050 chip support to `lua_module_imu`, including a standalone MPU6050 driver (`mpu6050.c`), configurable SDO pin level for I2C address selection, and accelerometer/gyroscope read APIs.

* Reorganized `lua_module_magnetometer` BMM350 driver sources into a dedicated `bmm350/` subdirectory for clearer module structure.

* Added a shared `http_reuse` component that wraps `esp_http_client_init`, `esp_http_client_cleanup`, and `esp_http_client_perform` to transparently reuse persistent HTTP clients across requests to the same endpoint.

* Added Kconfig options for HTTP client reuse, including feature enablement and configurable pool sizing.

* Added pooled client LRU eviction and one-time retry-on-failure handling for reused connections to reduce repeated connection setup and improve robustness.

* Wired `http_reuse` into `claw_core`, `cap_mcp_client`, `cap_web_search`, `cap_im_attachment`, `cap_im_feishu`, `cap_im_qq`, `cap_im_tg`, and `cap_im_wechat`.

* Added an indexed session history file header for Claw memory sessions to retain recent records by offset and rebuild session JSON without scanning legacy tab-delimited lines.

### Fix:

* Improved file descriptor management for WebSocket connections to reduce the issue where Web Chat did not receive reply messages. (https://github.com/espressif/esp-claw/issues/36)

* Adjusted `cap_im_qq` HTTP TX buffer sizing when HTTP reuse is enabled so reused clients keep compatible buffer settings.

* Fixed Feishu inbound image attachment saving to derive file extensions from the downloaded MIME type instead of assuming JPEG. 

## 2026-05-03

### Fix:

* Increased baudrate during flashing to speed up the process.

* Fixed an issue where the device could fail to enter network provisioning after online flashing. (https://github.com/espressif/esp-claw/issues/34)

* Fixed an issue where Web Chat might not receive reply messages from ESP-Claw.

## 2026-04-30

### Feature:

* Added `esp_SensairShuttle` board support for `edge_agent`, including board metadata, peripheral definitions, default board config, and device setup wiring.

* Added shared `app_claw` integration for the new Lua environmental sensor and magnetometer modules, including Kconfig, component dependencies, and Lua module registration.

* Added the `lua_module_environmental_sensor` module with Lua-facing sensor APIs and an `environmental_read.lua` example script.

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

* Added standard directory-based skill packaging with JSON frontmatter, generated skill sync tooling, and migrated shipped capability, memory, and Edge Agent skills from flat markdown files to `SKILL.md` directories.

* Added Lua module build and sync tooling for generated builtin Lua module/script skills, including module docs, script source metadata, shared Lua sync helpers, and the `lua_led_strip_switch` skill.

* Updated the ESP32-S3 DevKitC-1 breadboard UAC codec lifecycle to separate open/start/stop/close handling, track stream state, reuse device handles, and close devices on delete or disconnect.

### Change:

* Migrated selected Basic Demo updates into Edge Agent, including app configuration handling, HTTP UI updates, Lua module wiring, and default SDK configuration changes.

* Moved `dfrobot_k10` and other shipped board definitions into vendor-specific subdirectories, and relocated the LilyGO T-Display-S3 board assets from `basic_demo` to `edge_agent`.

* Reorganized Lua module documentation and examples into `docs/`, `lua_scripts/test`, and `lua_scripts/lib`, replacing per-module `skills_list.json` files with generated skill sources.

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
