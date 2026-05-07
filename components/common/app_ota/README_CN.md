# app_ota 使用说明

`app_ota` 是一个基于 `espressif/esp_ota_service` 的应用 OTA 组件，提供两条启动期升级链路：

- HTTP OTA：设备连上 STA 后，从 manifest + firmware URL 拉取升级包。
- Filesystem OTA：设备启动时从本地文件系统绝对路径读取固件（如 `/fatfs/ota/firmware.bin` 或 `/sdcard/firmware.bin`）。

组件依赖在 `idf_component.yml` 中声明：

- `espressif/esp_ota_service: "*"`

## 1. 对外 API

头文件：`include/app_ota.h`

- `app_ota_fs_boot_flow_at(const char *firmware_abs_path)`
  - 使用指定绝对路径执行一次 FS OTA（例如 `/sdcard/firmware.bin`）。
- `app_ota_fs_boot_flow(void)`
  - 使用 `CONFIG_APP_OTA_FS_FIRMWARE_ABS_PATH` 执行一次 FS OTA。
- `app_ota_http_boot_flow(bool sta_station_connected_to_ap)`
  - 处理 rollback pending-verify，并在 HTTP OTA 配置满足时执行一次 HTTP OTA。

注意：升级成功后设备会重启；开启 rollback 时，会根据联网状态执行确认或回滚。

## 2. Menuconfig 配置

路径：`Component config -> App OTA (esp_ota_service)`

### HTTP OTA 关键项

- `CONFIG_APP_OTA_HTTP_ENABLE`
- `CONFIG_APP_OTA_HTTP_RUN_AT_BOOT`
- `CONFIG_APP_OTA_HTTP_MANIFEST_URL`
- `CONFIG_APP_OTA_HTTP_FIRMWARE_URL`
- `CONFIG_APP_OTA_HTTP_CHUNK_TIMEOUT_MS`

说明：

- 只有在 `ENABLE=y` 且 URL 非空时，HTTP OTA 才会执行。
- 版本比较由 manifest checker 完成，要求候选版本高于当前运行版本。

### Filesystem OTA 关键项

- `CONFIG_APP_OTA_FS_ENABLE`
- `CONFIG_APP_OTA_FS_RUN_AT_BOOT`
- `CONFIG_APP_OTA_FS_FIRMWARE_ABS_PATH`（默认 `/fatfs/ota/firmware.bin`）
- `CONFIG_APP_OTA_FS_TRY_SDCARD_AT_BOOT`
- `CONFIG_APP_OTA_FS_SDCARD_FIRMWARE_ABS_PATH`（默认 `/sdcard/firmware.bin`）
- `CONFIG_APP_OTA_FS_READ_BUF_BYTES`
- `CONFIG_APP_OTA_FS_REQUIRE_NEWER_SEMVER`
- `CONFIG_APP_OTA_FS_CHECK_PROJECT_NAME`

## 3. 推荐接入顺序

参考 `application/edge_agent/main/main.c` 的调用时序：

1. `esp_board_manager_init()` 后，如启用 SD 卡优先启动链路，先调用
   `app_ota_fs_boot_flow_at(CONFIG_APP_OTA_FS_SDCARD_FIRMWARE_ABS_PATH)`。
2. `init_fatfs()` 后调用 `app_ota_fs_boot_flow()`，检查 SPI FAT 路径升级包。
3. Wi-Fi 启动并获取 STA 状态后，调用 `app_ota_http_boot_flow(sta_connected)`。

这样可以实现“先本地介质，再网络 OTA”的启动升级策略。

## 4. HTTP OTA 数据格式

manifest 至少包含这些字段（与 `esp_ota_service_checker_manifest` 对齐）：

- `version`：语义版本字符串，例如 `1.2.3`
- `project_name`：项目名（可选校验）
- `size`：固件大小
- `sha256`：固件哈希（可选但建议提供）

firmware URL 指向可下载的应用二进制（例如 `edge_agent.bin`）。

## 5. SDCard OTA 准备

若使用 SD 卡启动 OTA，请确保：

- 已将升级固件手动拷贝到 SD 卡，并命名为 `firmware.bin`，路径为 `/sdcard/firmware.bin`（或与配置路径一致）。
- `CONFIG_APP_OTA_FS_TRY_SDCARD_AT_BOOT=y`。
- 路径配置与实际挂载点一致。

## 6. 人工分步验证

### 6.1 SDCard OTA（人工分步）

1. 编译升级版本固件（higher semver）
   - 修改 `version.txt` 为升级版本（例如 `1.2.4`），然后：
   - `idf.py -B build_sd_upgrade build`
   - 产物：`build_sd_upgrade/edge_agent.bin`
2. 手动拷贝升级包到 SD 卡
   - 将 `build_sd_upgrade/edge_agent.bin` 拷贝到 SD 卡，并重命名为 `firmware.bin`。
   - 确保设备启动后可从 `CONFIG_APP_OTA_FS_SDCARD_FIRMWARE_ABS_PATH` 访问到该文件（默认 `/sdcard/firmware.bin`）。
3. 编译并刷入基线版本固件（lower semver）
   - 修改 `version.txt` 为基线版本（例如 `1.2.3`），然后：
   - `idf.py -B build_sd_base build flash -p /dev/ttyUSB3`
   - 首次验证建议先执行一次 `idf.py -B build_sd_base erase-flash -p /dev/ttyUSB3`。
4. 观察串口日志
   - 重点检查：检测到 SD 卡固件、开始 OTA、`OTA finished`、重启后版本变为升级版本。

### 6.2 HTTP OTA（使用 esp_ota_service tools 手动分步）

1. 编译升级版本固件（higher semver）
   - 修改 `version.txt` 为升级版本（例如 `1.2.4`）：
   - `idf.py -B build_http_upgrade build`
   - 产物：`build_http_upgrade/edge_agent.bin`
2. 准备 `esp_ota_service/tools` 的 `firmware_samples`
   - 目录：`application/edge_agent/managed_components/espressif__esp_ota_service/tools/firmware_samples`
   - 复制升级固件到该目录，例如命名为 `edge_agent_v1.2.4.bin`
   - 生成/更新 `manifest.json`，至少包含 `version`、`size`、`sha256`、`url`
3. 启动 HTTP 文件服务（使用 ota_service 自带脚本）
   - 脚本：`ota_http_serve.py`
   - 示例命令：
     ```bash
     python3 application/edge_agent/managed_components/espressif__esp_ota_service/tools/ota_http_serve.py \
       --bin edge_agent_v1.2.4.bin \
       --port 18070 \
       --update-manifest-url
     ```
4. 编译并刷入基线版本固件（lower semver）
   - 修改 `version.txt` 为基线版本（例如 `1.2.3`）
   - 在 menuconfig/sdconfig 中配置：
     - `CONFIG_APP_OTA_HTTP_ENABLE=y`
     - `CONFIG_APP_OTA_HTTP_RUN_AT_BOOT=y`
     - `CONFIG_APP_OTA_HTTP_MANIFEST_URL=http://<HOST_IP>:18070/manifest.json`
     - `CONFIG_APP_OTA_HTTP_FIRMWARE_URL=http://<HOST_IP>:18070/firmware.bin`
   - 然后执行：`idf.py -B build_http_base build flash -p /dev/ttyUSB3`
5. 观察串口日志
   - 重点检查：HTTP 拉取成功、版本检查通过、`OTA finished`、重启后版本升级。

提示：`erase-flash` 不是必需步骤，但首次验证或切换配置场景建议执行，以减少 NVS 历史状态干扰。
