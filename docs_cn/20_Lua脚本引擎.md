# 20 Lua 脚本引擎

## 概述

esp-claw 通过 `cap_lua` 组件提供一个轻量级的 Lua 5.x 脚本执行环境。Lua 脚本可以调用系统能力（capability）、发布事件，并访问各类硬件驱动和系统模块。

---

## 1. Lua VM 的创建与销毁策略

**每次执行都创建一个全新的 `lua_State`**，执行完成后立即关闭（`lua_close(L)`）。这是一种无状态的沙盒模型。

`cap_lua_runtime_execute_file()` 的核心流程：

```c
L = luaL_newstate();          // 1. 创建全新 VM
luaL_openlibs(L);             // 2. 开放标准库
cap_lua_load_registered_modules(L);  // 3. 注册所有预注册 C 模块
cap_lua_add_script_dir_to_package_path(L, path);  // 4. 设置 package.path
cap_lua_set_args_global(L, args_json);   // 5. 注入 args 全局变量
lua_sethook(L, cap_lua_timeout_hook, LUA_MASKCOUNT, 100);  // 6. 设置超时钩子
status = luaL_dofile(L, path); // 7. 执行脚本文件
cap_lua_run_exit_cleanups(L);  // 8. 运行退出清理函数
lua_close(L);                  // 9. 销毁 VM
```

### 同步执行 vs 异步执行

| 模式 | 接口 | 特点 |
|------|------|------|
| 同步 | `cap_lua_run_script()` | 阻塞等待，默认超时 60 秒 |
| 异步 | `cap_lua_run_script_async()` | 提交到异步 Job 队列，立即返回 Job ID |

异步执行器是一个独立的 FreeRTOS 任务，使用 `CAP_LUA_ASYNC_MAX_CONCURRENT = 4` 的并发限制，最多同时保留 `CAP_LUA_ASYNC_MAX_JOBS = 16` 个 Job 记录。

每个 Job 持有自己独立的 `lua_State`，不共享状态。

---

## 2. 沙盒约束

### 开放的标准库

调用 `luaL_openlibs(L)` 会开放 **Lua 全部标准库**，包括：

| 库 | 功能 |
|----|------|
| `base` | print、pairs、ipairs、type 等基础函数 |
| `package` | require、package.path |
| `string` | 字符串操作 |
| `table` | 表操作 |
| `math` | 数学函数 |
| `io` | 文件 I/O（**注意：已开放**）|
| `os` | 系统调用（**注意：已开放**）|
| `coroutine` | 协程 |
| `debug` | 调试接口 |

> **当前实现未禁用任何标准库**。沙盒安全主要通过文件路径校验实现（见下文）。

### 路径安全约束

路径验证在 C 层强制执行：

- 脚本路径必须是绝对路径（`/` 开头）
- 不允许 `..` 路径穿越
- 必须以 `.lua` 结尾
- 调用 `cap_lua_resolve_path()` 时，相对路径被限制在 `base_dir` 下

### 自定义 print 函数

脚本中的 `print()` 被替换为捕获版本，输出同时写入：
1. 内部输出缓冲区（返回给调用者）
2. 异步 Job 的滚动日志缓冲
3. 标准输出（`stdout`）

### 超时机制

每 100 条 Lua 指令触发一次 `cap_lua_timeout_hook`，检查：
1. 协作式取消标志（`stop_requested`）
2. 墙钟截止时间（`deadline_us`，基于 `esp_timer_get_time()`）

超时触发 `luaL_error(L, "execution timed out")`，映射到 C 层的 `ESP_FAIL`。

---

## 3. 可用的 Lua 模块

`lua_modules/` 目录下包含以下模块：

### 核心系统模块

| 模块目录 | 注册名 | 功能 |
|---------|--------|------|
| `lua_module_call_capability` | `capability` | 调用 claw_cap 能力 |
| `lua_module_event_publisher` | `event_publisher` | 发布事件到事件路由器 |
| `lua_module_json` | `json` | JSON 编解码 |
| `lua_module_system` | `system` | 系统信息查询 |
| `lua_module_storage` | `storage` | 文件系统存储 |
| `lua_module_delay` | `delay` | 延时控制 |
| `lua_module_thread` | `thread` | 线程/协程管理 |

### 硬件驱动模块

| 模块目录 | 功能 |
|---------|------|
| `lua_driver_gpio` | GPIO 控制 |
| `lua_driver_adc` | ADC 采样 |
| `lua_driver_i2c` | I2C 通信 |
| `lua_driver_uart` | UART 通信 |
| `lua_driver_mcpwm` | 电机 PWM |
| `lua_driver_pcnt` | 脉冲计数 |
| `lua_driver_touch` | 触摸感应 |

### 外设与功能模块

| 模块目录 | 功能 |
|---------|------|
| `lua_module_button` | 按钮输入 |
| `lua_module_knob` | 旋钮输入 |
| `lua_module_led_strip` | LED 灯带 |
| `lua_module_lcd` | LCD 显示 |
| `lua_module_lcd_touch` | LCD 触摸 |
| `lua_module_display` | 显示控制 |
| `lua_module_lvgl` | LVGL GUI |
| `lua_module_camera` | 摄像头 |
| `lua_module_image` | 图像处理 |
| `lua_module_vision` | 视觉识别 |
| `lua_module_audio` | 音频处理 |
| `lua_module_ble_hid` | BLE HID |
| `lua_module_ir` | 红外收发 |
| `lua_module_imu` | IMU 传感器 |
| `lua_module_environmental_sensor` | 环境传感器 |
| `lua_module_magnetometer` | 磁力计 |
| `lua_module_fuel_gauge` | 电量计 |
| `lua_module_http_server` | HTTP 服务器 |
| `lua_module_sci` | SCI 科学接口 |
| `lua_module_board_manager` | 板级管理 |

---

## 4. 如何从 Lua 调用 claw_cap

模块名：`capability`，注册函数 `luaopen_capability`（`lua_module_call_capability/src/lua_module_capability.c`）。

### 接口

```lua
local ok, output, err = capability.call(cap_name, payload, opts)
```

| 参数 | 类型 | 说明 |
|------|------|------|
| `cap_name` | string | 能力名称或 ID |
| `payload` | table/string/nil | 输入参数（自动序列化为 JSON；传字符串时须为合法 JSON）|
| `opts` | table/nil | 可选选项（见下表）|

**opts 字段：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `session_id` | string | 会话 ID，若不传则从全局 `args.session_id` 继承 |
| `channel` | string | 来源频道 |
| `chat_id` | string | 聊天 ID |
| `source_cap` | string | 来源能力标识 |
| `max_output_bytes` | number | 输出缓冲区大小（默认 64 KB，最大 256 KB）|

**返回值：**
- `ok`（boolean）：调用是否成功
- `output`（string/nil）：能力输出文本
- `err`（string/nil）：失败时的错误码字符串（如 `"ESP_ERR_NOT_FOUND"`）

**示例：**

```lua
local capability = require("capability")

local ok, result, err = capability.call("my_sensor", {unit = "celsius"}, {
    session_id = args.session_id,
})
if ok then
    print("Temperature: " .. result)
else
    print("Error: " .. (err or "unknown"))
end
```

### caller 标识

通过 Lua 调用时，`ctx.caller` 被固定设置为 `CLAW_CAP_CALLER_SYSTEM`（非 AGENT），因此不受 LLM 可见性过滤限制，可以调用任意已注册且 started 状态的能力。

---

## 5. 如何从 Lua 发布事件

模块名：`event_publisher`，注册函数 `luaopen_event_publisher`（`lua_module_event_publisher/src/lua_module_event_publisher.c`）。

### 接口一览

```lua
local ep = require("event_publisher")

-- 通用事件（完整字段控制）
ep.publish({ source_cap, event_type, ... })

-- 快捷：发布消息事件
ep.publish_message({ source_cap, channel, chat_id, text, ... })

-- 快捷：发布触发事件
ep.publish_trigger({ source_cap, event_type, event_key, payload_json })
```

### publish() 完整字段

| 字段 | 必选 | 类型 | 说明 |
|------|------|------|------|
| `source_cap` | 是 | string | 来源能力名 |
| `event_type` | 是 | string | 事件类型（如 `"trigger"`、`"message"`）|
| `event_id` | 否 | string | 自定义事件 ID，缺省自动生成 `lua-<ms>` |
| `session_policy` | 否 | string | `"chat"`/`"trigger"`/`"global"`/`"ephemeral"`/`"nosave"` |
| `chat_id` | 否 | string | 目标聊天 ID |
| `text` | 否 | string | 消息文本 |
| `payload` | 否 | table | 载荷（自动序列化为 JSON）|
| `payload_json` | 否 | string | 已序列化的载荷 JSON |
| `timestamp_ms` | 否 | integer | 时间戳，缺省取当前时间 |

**session_policy 默认规则：**
- 未指定时，若 `event_type == "trigger"` 则默认 `TRIGGER`，否则默认 `CHAT`

**示例：**

```lua
local ep = require("event_publisher")

-- 发布一个 trigger 事件
ep.publish_trigger({
    source_cap = "cap_sensor",
    event_type = "temperature_alert",
    event_key  = "overheat",
    payload    = {value = 85.0, unit = "celsius"},
})

-- 发布一条消息给指定频道
ep.publish_message({
    source_cap = "cap_monitor",
    channel    = "telegram",
    chat_id    = "123456",
    text       = "System alert: temperature exceeded threshold",
})
```

---

## 6. 脚本的输入/输出约定

### 输入（args 全局变量）

执行脚本前，运行时将 `args_json` 解析后注入为全局变量 `args`：

```c
root = cJSON_Parse(args_json);
cap_lua_push_json_value(L, root);
lua_setglobal(L, "args");
```

- 若 `args_json` 为空或无效，`args` 设置为空 table `{}`
- `args` 是一个普通 Lua table，JSON 对象的字段直接映射为 table 字段

在脚本中：

```lua
-- 读取传入参数
local session = args.session_id
local input   = args.input_text or "default"
```

### 输出

脚本输出通过重写的 `print()` 函数收集，每次 `print()` 的所有参数按 `\t` 分隔、末尾追加 `\n`，写入一个固定大小缓冲区：

| 场景 | 缓冲区大小 |
|------|-----------|
| 同步调用（`run_script`）| `CAP_LUA_OUTPUT_SIZE = 4 * 1024` B |
| 异步 Job 日志 | `CAP_LUA_ASYNC_LOG_DEFAULT_BYTES = 4 * 1024` B（可配置最大 16 KB）|

超出缓冲区后追加 `[output truncated]`。

若脚本运行成功但没有任何输出，自动追加：`Lua script completed with no output.\n`

---

## 7. 错误处理：Lua 运行时错误到 esp_err_t

| 情况 | Lua 层 | C 层返回值 |
|------|--------|-----------|
| 脚本正常完成 | `LUA_OK` | `ESP_OK` |
| 任何 Lua 错误（语法错误、运行时错误、`error()` 调用）| `status != LUA_OK` | `ESP_FAIL` |
| 超时 | `luaL_error(L, "execution timed out")` | `ESP_FAIL` |
| 协作取消 | `luaL_error(L, "stopped by user")` | `ESP_FAIL` |
| 脚本文件不存在 | 提前返回 | `ESP_ERR_NOT_FOUND` |
| 脚本文件超过 `CAP_LUA_MAX_SCRIPT_SIZE = 16 KB` | 提前返回 | `ESP_ERR_INVALID_SIZE` |
| 内存分配失败（`luaL_newstate`）| 提前返回 | `ESP_ERR_NO_MEM` |

所有 `ESP_FAIL` 情况下，错误消息会被追加到输出缓冲区末尾，格式为 `ERROR: <lua error message>\n`。

Lua 内部通过 `luaL_error()` 的模块调用错误示例：

```c
// lua_module_event_publisher.c
luaL_error(L, "event publish failed: %s", esp_err_to_name(err));
```

该调用使 Lua 状态机进入错误，最终由 `luaL_dofile` 的非 `LUA_OK` 返回值被 C 层捕获并转换为 `ESP_FAIL`。

---

## 8. 关键常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `CAP_LUA_MAX_SCRIPT_SIZE` | 16 KB | 单个脚本文件上限 |
| `CAP_LUA_OUTPUT_SIZE` | 4 KB | 同步执行输出缓冲 |
| `CAP_LUA_SYNC_DEFAULT_TIMEOUT_MS` | 60,000 ms | 同步执行超时 |
| `CAP_LUA_ASYNC_DEFAULT_TIMEOUT_MS` | 0（无限）| 异步执行超时 |
| `CAP_LUA_ASYNC_MAX_JOBS` | 16 | 最大 Job 记录数 |
| `CAP_LUA_ASYNC_MAX_CONCURRENT` | 4 | 最大并发 Job 数 |
| `CAP_LUA_MAX_MODULES` | 32 | 最大可注册模块数 |
| `LUA_MODULE_CAPABILITY_DEFAULT_OUTPUT_SIZE` | 64 KB | capability.call 默认输出缓冲 |
| `LUA_MODULE_CAPABILITY_MAX_OUTPUT_SIZE` | 256 KB | capability.call 最大输出缓冲 |
