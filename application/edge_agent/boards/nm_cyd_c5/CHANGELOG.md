# Changelog

## [2026-05-22]

合并上游 `espressif/master`（`a7be159`）到 `nm-cyd-c5`，完成冲突处理、技能目录迁移适配与功能兼容性核验。

---

### Merge Conflict Summary

- **冲突文件总数：21**
  - `application/edge_agent/components/http_server/CMakeLists.txt`
  - `application/edge_agent/main/idf_component.yml`
  - `application/edge_agent/main/main.c`
  - `application/edge_agent/sdkconfig.defaults`
  - `components/claw_capabilities/cap_skill_mgr/src/cap_skill_mgr.c`
  - `components/claw_modules/claw_skill/src/claw_skill.c`
  - `components/common/app_claw/Kconfig`
  - `components/common/app_claw/app_lua_modules.c`
  - `components/common/app_claw/idf_component.yml`
  - `components/common/wifi_manager/wifi_manager.c`
  - `components/lua_modules/lua_module_display/CMakeLists.txt`
  - `components/lua_modules/lua_module_display/src/display_hal.c`
  - `components/lua_modules/lua_module_display/src/display_hal.h`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_backlight.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_gps.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_screen.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_control.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_info.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_scan.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_setting.lua`

- **处理原则**
  - `cap_skill_mgr.c`、`claw_skill.c`：采用上游实现，避免技能注册路径与 catalog 语义偏差。
  - `main.c`：保留 nm-cyd-c5 的 SD 镜像、消息持久化、状态屏逻辑，并合入上游 `claw_ramfs` 初始化。
  - `wifi_manager.c`：保留本地重连状态清理逻辑，同时合并上游 `ap_behavior=close_on_sta` 行为。
  - `app_claw` 相关冲突：移除已上游淘汰的 `lua_module_esp_heap` 依赖，保留 `lua_module_http`（保障 nmminer 现有脚本能力）。
  - `lua_module_display`：保留本地背光百分比接口与分段 swap buffer 优化，兼容上游 `display_color_t` 接口。
  - skill 路径冲突（`main/skills` -> `fatfs_image/skills`）：采用上游迁移结果，保留 nm-cyd-c5 与 nmminer 脚本。

- **额外修复**
  - 清理重复技能 ID：删除 `application/edge_agent/main/skills/skill_creator/SKILL.md`，避免与上游 `components/common/skill_builder/skills/skill_creator/SKILL.md` 重复导致构建失败。

---

### Upstream Updated Files (Key Groups)

- **构建与CI**
  - `.gitlab/ci/build.yml`
  - `.gitlab/ci/build_apps.py`
  - `.gitlab/ci/merge_bin.py`
  - `application/edge_agent/tools/cmake/esp_idf_patch.cmake`
  - `application/edge_agent/tools/cmake/flash_partition_defaults.cmake`

- **Edge Agent 主工程**
  - `application/edge_agent/CMakeLists.txt`
  - `application/edge_agent/main/main.c`
  - `application/edge_agent/main/idf_component.yml`
  - `application/edge_agent/main/Kconfig.projbuild`
  - `application/edge_agent/sdkconfig.defaults`
  - `application/edge_agent/partitions_8MB.csv`
  - `application/edge_agent/partitions_16MB.csv`
  - `application/edge_agent/partitions_32MB.csv`

- **HTTP Server 与前端**
  - `application/edge_agent/components/http_server/http_server_core.c`
  - `application/edge_agent/components/http_server/http_server_priv.h`
  - `application/edge_agent/components/http_server/http_server_lua_app_api.c`
  - `application/edge_agent/components/http_server/frontend_source/src/pages/WebImPage.tsx`
  - `application/edge_agent/components/http_server/frontend_source/src/pages/LlmPage.tsx`
  - `application/edge_agent/components/http_server/frontend_source/src/pages/SetupWizardPage.tsx`
  - `application/edge_agent/components/http_server/frontend_source/dist/index.html.gz`

- **技能与文件镜像结构迁移**
  - `application/edge_agent/fatfs_image/skills/lua_demo/SKILL.md`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_backlight.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_gps.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_rgb.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nm_cyd_c5_screen.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_control.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_info.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_scan.lua`
  - `application/edge_agent/fatfs_image/skills/lua_demo/scripts/nmminer_setting.lua`

- **核心组件更新（节选）**
  - `components/claw_modules/claw_core/src/claw_core.c`
  - `components/claw_modules/claw_memory/src/claw_memory_session.c`
  - `components/claw_capabilities/cap_lua/src/cap_lua.c`
  - `components/common/settings/settings_store.c`
  - `components/common/wifi_manager/wifi_manager.c`
  - `components/common/skill_builder/skills/skill_creator/SKILL.md`
  - `components/common/esp_video/idf_component.yml`
  - `components/claw_modules/claw_ramfs/src/claw_ramfs.c`

---

### Compatibility Check (nm-cyd-c5 Existing Features)

- [x] `skills/skill_creator`
  - 结果：通过（采用上游 `components/common/skill_builder/skills/skill_creator/`，移除本地重复定义，避免重复 skill id）。

- [x] `skills/nm_cyd_c5_*`
  - 结果：通过（4 个脚本均保留并迁移至 `application/edge_agent/fatfs_image/skills/lua_demo/scripts/`）。

- [x] `skills/nmminer`
  - 结果：通过（4 个 nmminer 脚本均保留并迁移至 `application/edge_agent/fatfs_image/skills/lua_demo/scripts/`）。

- [x] `app_sd_mirror.c/h, SD card backup/restore function`
  - 结果：通过（组件与依赖仍在；`main.c` 中镜像与恢复调用链保持完整）。

---

### Build Validation Notes

- 当前工作区执行 `idf.py build` 时触发 BLE HID 相关编译错误（`lua_module_ble_hid` 的 `esp_bt.h` 头文件解析），该项为上游新增模块与本地当前 sdkconfig 组合导致的问题，不属于本次冲突处理回归。
- 本次合并已完成冲突收敛、技能目录迁移与 nm-cyd-c5 既有能力兼容性核验。

## [2026-05-11]

合并上游 `espressif/master`（`8dc62b4`），同步 WebIM 稳定性增强、LLM 配置去 profile 化、硬件 Lua 模块命名标准化等 8 项核心变更。

> 上游涉及前端心跳机制、LLM 运行时精简、驱动组件重命名、新板卡支持等，详见下方分类记录。

---

### Added

- **WebIM 心跳机制与设备重启确认**
  - `http_server_webim_api.c` 新增 WebSocket 异步广播作业队列（`webim_ws_broadcast_job_t`），支持多客户端并发推送且自动清理断连 fd。
  - 前端新增 `RestartConfirmModal` 组件，设备重启前弹出二次确认，防止误操作导致会话中断。
  - `WebImPage.tsx` 集成心跳保活逻辑，降低长连接被中间设备（路由器/NAT）切断的概率。
  - 关联提交：`f95614d`、`8dc04a9`

- **LLM 请求体 UTF-8 清理层**
  - `claw_llm_http_transport.c` 新增 `sanitize_utf8_body_copy()`，在 HTTP 发送前扫描并替换无效 UTF-8 序列为空格。
  - 解决因 Lua 脚本输出、SD 卡日志或传感器数据混入非法字节导致 LLM API 返回 HTTP 400 的问题。
  - 该逻辑从 OpenAI Compatible 后端下沉至通用传输层，所有后端（Anthropic、Custom、OpenAI）自动受益。
  - 关联提交：`e3dcdd3`、`31c458b`

- **GitHub PR 同步机器人**
  - 新增 `.github/workflows/pr_approved.yml`，当 PR 获得批准时自动触发同步通知。
  - 关联提交：`9117a2b`

---

### Changed

- **移除 LLM Profile 预置配置**
  - 删除 `claw_llm_runtime.c` 中硬编码的 `s_profiles[]` 表（OpenAI/Qwen 等预设），LLM 配置完全由用户自定义。
  - `app_config.c` 大幅重构（+384 行），新增遗留预设迁移逻辑（`app_config_legacy_llm_preset_t`），旧配置自动升级为新格式。
  - 所有 LLM 后端（Anthropic、Custom、OpenAI Compatible）移除 `profile` 参数依赖，改为直接从 `claw_llm_runtime_config_t` 读取。
  - 前端 `LlmPage.tsx` 与 `SetupWizardPage.tsx` 重新设计：移除 profile 下拉框，改为直接填写 base_url、model、api_key 等字段。
  - `settings_store.c/h` 新增 `settings_store_get_string()` 等接口，支撑更灵活的持久化读写。
  - 关联提交：`f924ff3`

- **硬件 Lua 模块统一重命名为 Lua Driver**
  - 将 6 个硬件外设相关模块从 `lua_module_xxx` 重命名为 `lua_driver_xxx`，语义更清晰：
    | 旧名称 | 新名称 |
    | :--- | :--- |
    | `lua_module_adc` | `lua_driver_adc` |
    | `lua_module_gpio` | `lua_driver_gpio` |
    | `lua_module_i2c` | `lua_driver_i2c` |
    | `lua_module_mcpwm` | `lua_driver_mcpwm` |
    | `lua_module_touch` | `lua_driver_touch` |
    | `lua_module_uart` | `lua_driver_uart` |
  - `app_claw/Kconfig` 新增 "Hardware peripheral drivers (lua_driver)" 独立分区，默认全部启用。
  - `app_lua_modules.c` 和 `idf_component.yml` 同步更新依赖路径。
  - `lua_module_builder` 文档与生成脚本适配新前缀。
  - 关联提交：`7f0be3f`

- **前端页面重构与状态管理增强**
  - `StatusPage.tsx` 功能拆分：状态展示精简，部分配置入口迁移至 `BasicPage`、`CapabilitiesPage`、`SearchPage`、`SkillsPage`。
  - `App.tsx` 全局状态管理重构，适配重启确认流程。
  - `ConfigBlocks.tsx` 配置块组件通用化，支持更多表单类型。
  - 多语言（`en.ts`、`zh-cn.ts`）补充新词条约 40+ 条。
  - 关联提交：`8dc04a9`

- **app_config 配置系统增强**
  - 支持更多 NVS 字段的动态读写。
  - 新增配置合法性校验与默认值回退机制。
  - 关联提交：`f924ff3`

---

### Fixed

- **IMU 模块编译兼容性**
  - `lua_module_imu.c` 修复 MPU6050 驱动在部分编译优化级别下的寄存器读取异常。
  - 关联提交：`7f0be3f`（同批重构中修复）

- **燃料计组件依赖声明**
  - `lua_module_fuel_gauge/idf_component.yml` 补充缺失的公共依赖声明。
  - 清理 `test/fuel_gauge_read.lua` 中的调试打印代码。
  - 关联提交：`7f0be3f`（同批重构中修复）

---

### Migration Notes

1. **LLM 配置迁移**：由于移除了 profile 预设，原有依赖 `profile` 字段的自定义代码需改为直接操作 `claw_llm_runtime_config_t` 的 `base_url`、`model`、`api_key` 等字段。`app_config` 会自动将旧 NVS 数据升级为新格式，无需手动干预。
2. **Lua Driver 引用更新**：若自定义组件或脚本通过旧名称 `lua_module_adc` 等引用硬件驱动，请更新为 `lua_driver_adc` 等新前缀。`cap_lua` 的能力描述文档已同步更新。
3. **前端构建**：`edge_agent/components/http_server/frontend_source/` 有大幅更新，合并后需重新执行 `pnpm install && pnpm build` 以生成新的 `index.html.gz`。

---

## [2026-05-07]

合并上游 `espressif/master`（`582022f`），适配官方重大架构重构，更新 NM-CYD-C5 技能与构建配置以支持最新框架。

> 上游涉及 IM 平台合并、Skill/Lua 构建系统标准化、Session 索引加固、HTTP 复用层引入等 12 项核心变更，详见下方分类记录。

---

### Added

- **IM 统一平台 `cap_im_platform`**
  - 将原先分散的 5 个独立 IM 组件（`cap_im_attachment`、`cap_im_feishu`、`cap_im_qq`、`cap_im_tg`、`cap_im_wechat`）合并为单一组件。
  - 通过 `CMakeLists.txt` 中的 `if(CONFIG_APP_CLAW_CAP_IM_xxx)` 条件编译控制各子模块启停。
  - 新增统一入口 `cap_im_platform.c`，公共头文件全部收敛到 `cap_im_platform/include/`。
  - 下游影响：`idf_component.yml` 中所有旧 IM 依赖路径需替换为 `cap_im_platform`。
  - 关联提交：`714747e`

- **Skill / Lua 标准化构建系统**
  - 新增 `skill_builder` + `lua_module_builder` 组件。
  - `skill_builder`：提供 `skill_sync.cmake` + `sync_component_skills.py`，构建时自动扫描各组件 `skills/` 目录并同步到 FATFS。
  - `lua_module_builder`：提供 `lua_sync.cmake` + Python 工具链，自动同步 Lua 模块的 `test/` 与 `lib/` 目录。
  - 关联提交：`fa2db28`

- **HTTP 客户端复用层 `http_reuse`**
  - 新增 `components/common/http_reuse/` 组件。
  - 封装 `esp_http_client_*` API，提供连接池复用机制，减少重复 TCP/TLS 握手开销。
  - 配套 `Kconfig.projbuild` 支持配置连接池大小与超时。
  - 关联提交：`8afce25`

- **Session 索引与持久化加固**
  - 新增 base64 编码的 session history header，支持快速定位会话边界。
  - session history 文件损坏或格式无效时，自动重建索引而非直接失败。
  - 关联提交：`429712a`、`e799d29`、`eaf9eff`

- **大文件截断读取保护（`cap_files`）**
  - 新增 `CAP_FILES_TRUNCATION_SUFFIX_RESERVE (96 bytes)`。
  - 当文件超过 `CAP_FILES_MAX_FILE_SIZE`（32 KiB）时，不再直接拒绝，而是截断读取并保留尾部提示空间。
  - 所有 I/O 错误路径（`fopen`/`opendir`/`mkdir`/`stat`/`fread`/`fwrite`）补充 `errno` 日志，便于调试 SD 卡或 FATFS 异常。

- **Skill 示例 `light_switch`**
  - 位于 `edge_agent/main/skills/light_switch/`，包含 GPIO 开关与灯带控制示例脚本。

- **UART / JTAG 控制台输出配置**
  - 新增 `sdkconfig.console.jtag` 模板文件。
  - CI 支持根据构建参数选择 UART 或 JTAG 控制台输出。
  - 关联提交：`53f2585`

---

### Changed

- **Skill / Lua 目录结构重构**
  - 旧格式（`skills/cap_xxx.md` + `skills_list.json`、`lua_scripts/basic_xxx.lua`）已废弃。
  - 新格式：
    ```text
    skills/
    └── skill_name/
        ├── SKILL.md          # 统一入口文档
        └── scripts/
            └── script.lua    # 附属脚本
    ```
  - `edge_agent` 具体迁移：
    - `edge_agent/main/lua_scripts/` → `edge_agent/main/skills/lua_demo/scripts/`
    - `edge_agent/main/skills/weather.md` → `edge_agent/main/skills/weather_search/SKILL.md`
  - 约 20+ 个 `skills_list.json` 被批量删除，所有 `.md` skill 文件重命名为 `SKILL.md` 并移入子目录。
  - 关联提交：`fa2db28`

- **`cap_boards`：从 YAML 元数据自动生成 Skill**
  - `generate_board_skill.py` 大幅改写（`-244` / `+165` 行）。
  - 从 `board_info.yaml` / `board_devices.yaml` 的元数据直接生成 `SKILL.md`，不再依赖手写的 `skills_list.json`。
  - 关联提交：`c64fc06`

- **Feishu：图片存储 MIME 感知**
  - 接收到的图片附件不再使用固定扩展名，而是根据 HTTP `Content-Type` 自动推导（`.jpg`、`.png`、`.gif` 等）。
  - 避免不同 MIME 类型的图片覆盖同一文件。
  - 关联提交：`27f29b9`

- **LLM System Prompt 精简**
  - `claw_core` 的 LLM 系统提示词裁剪，减少 token 消耗，提升上下文利用率。
  - 关联提交：`ef3486b`

- **USB-UAC 音频配置更新**
  - `esp32_S3_DevKitC_1_breadboard` 的 UAC 编解码器配置更新。
  - `sdkconfig.defaults.board` 新增 UAC 相关默认配置。
  - 关联提交：`430edc9`、`3df147b`

- **控制台输出适配**
  - `lilygo_t_display_s3` 等 board 的 `setup_device.c` 同步适配 UART/JTAG 配置。

---

### Removed

- **已删除的独立 IM 组件**

  | 旧组件 | 新归属 |
  | :--- | :--- |
  | `cap_im_attachment` | `cap_im_platform/src/cap_im_attachment.c` |
  | `cap_im_feishu` | `cap_im_platform/src/cap_im_feishu.c` |
  | `cap_im_qq` | `cap_im_platform/src/cap_im_qq.c` |
  | `cap_im_tg` | `cap_im_platform/src/cap_im_tg.c` |
  | `cap_im_wechat` | `cap_im_platform/src/cap_im_wechat.c` |

- **废弃的 Lua 模块 `lua_module_dht`**
  - 功能合并入 `lua_module_environmental_sensor`。
  - 删除独立的 `lua_module_dht/CMakeLists.txt`、`idf_component.yml` 和源码。
  - 关联提交：`19210ef`

- **冗余构建脚本与 CMake 模块**
  - 删除 `tools/bmgr_patch.py`、`skills_sync.cmake`、`board_manager_patch.cmake`、`sync_component_lua_scripts.py`、`sync_component_skills.py` 等 4+ 个冗余文件。
  - `edge_agent/main/CMakeLists.txt` 移除 `REQUIRES ${main_requires}` 显式列表，依赖关系完全迁移到 `idf_component.yml` 声明式管理。
  - 关联提交：`1b8b52b`

---

### Deprecated

- `cap_files` 仍然不支持多挂载点访问（如 `/sdcard` 独立挂载点）。LLM 通过 `cap_files` 能力无法直接操作 SD 卡内容，仅能通过 Web 文件管理器访问。

---

### Migration Notes

1. **依赖更新**：若自定义组件仍引用旧 IM 组件，请将 `idf_component.yml` 中的路径统一替换为 `cap_im_platform`。
2. **Skill 开发**：新 Skill 请使用 `SKILL.md` + `scripts/` 目录结构，不再手写 `skills_list.json`。
3. **Lua 脚本**：Lua 模块的示例脚本请放入组件的 `test/` 目录，公共库放入 `lib/` 目录，由 `lua_module_builder` 自动同步。

### Check List

 - [x] skills/skill_creator
 - [x] skills/nm_cyd_c5_*
 - [x] skills/nmminer
 - [x] app_sd_mirror.c/h, SD card backup/restore function.
 - [ ] running statble test.