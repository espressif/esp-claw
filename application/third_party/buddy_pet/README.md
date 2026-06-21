# Buddy Pet Guide

## EN

Buddy Pet is an ESP-Claw product example under
`application/third_party/buddy_pet/`. It targets ESP-Ditto style devices and
shows how to combine the ESP-Claw agent runtime, WeChat/IM control, local web
setup, downloadable buddy packages, and the Match Watch football UI in one
firmware application.

### What It Does

- Runs ESP-Claw event router, agent core, Lua skills, memory, and capabilities.
- Supports WeChat/IM binding, exact command routing, and natural-language agent requests.
- Opens Match Watch for football schedules, team focus, reminders, and live or simulated data.
- Downloads, installs, selects, and animates buddy packages.
- Provides local web setup for Wi-Fi, WeChat binding, LLM, search keys, and timezone.
- Uses the board LCD and touch controller for QR setup, Match Watch navigation, and buddy interaction.

### IM Chat

Buddy Pet accepts both exact IM commands and natural-language prompts.

Common prompts:

- `/new`: start a new persistent chat session.
- `打开比赛`: open Match Watch UI directly.
- `打开模拟赛事`: start the simulated Match Watch provider and enable reminders.
- `关闭模拟赛事`: stop the simulated provider and sync back to the current buddy.
- `开启比赛提醒` / `关闭比赛提醒`: enable or disable Match Watch reminders.
- `刷新比赛`: refresh Match Watch data while the UI is running.
- `根据当前伙伴打开比赛看板`: open Match Watch and sync from the selected buddy profile.
- `关注 Brazil World Cup`: follow a team through the Match Watch provider.
- `下载这个 buddy <url>` / `下载 messi`: download and select a buddy package.
- `切换到 son-heung-min`: select an installed buddy.
- `让兄弟跳一下`: trigger a buddy action.

For the full IM prompt, Lua entry, and router-event reference, see:

```text
application/third_party/buddy_pet/fatfs_image/skills/LUA_SKILL_COMMANDS.md
```

### Local Setup and Touch Controls

Local setup:

- Runs the local HTTP configuration service.
- When STA Wi-Fi is not connected, the device shows a Wi-Fi QR code for its
  provisioning AP, for example `esp-claw-xxxx`.
- Scan it with the phone camera or system QR scanner. The phone should recognize
  it as a Wi-Fi network QR code and prompt to join the device AP. After joining
  that AP, use the captive portal or local setup page to submit the target STA
  Wi-Fi credentials.
- It is recommended to configure the LLM provider and API key on the same setup
  page so natural-language IM requests can use the agent runtime.
- After IM binding is ready, LLM can also be checked or configured from chat:
  send `/llm status`, or use a command such as
  `/llm setup deepseek sk-xxx deepseek-v4-pro`.
- After Wi-Fi is submitted and connected, the device shows a WeChat binding QR
  code labeled "WeChat device binding" when WeChat credentials are not ready.
- After WeChat binding completes, the credentials are saved through the app
  config subsystem and the default product mode opens Match Watch.
- The setup page also exposes Wi-Fi, WeChat, LLM provider, IM credentials,
  search key, timezone, file, and system settings.
- When STA Wi-Fi is unavailable, the device AP and captive setup page remain
  available for provisioning.

Match Watch:

- When no team has been chosen manually, Match Watch follows the current buddy's
  profile. For example, a South Korea buddy will open South Korea matches.
- If you follow another team in chat, such as `关注 Brazil World Cup`, that team
  becomes your choice and is kept after reboot. Changing or reloading the buddy
  will not replace it unless you explicitly choose a new buddy/team again.
- Match Watch time page tap: enter the team page.
- Match Watch team/detail page tap: switch between team and detail views.
- Match Watch left swipe: next match.
- Match Watch right swipe: previous match.
- Match Watch up swipe: next stage with matches.
- Match Watch down swipe: previous stage with matches.
- Buddy drag: on pages where the buddy accepts touch, drag the buddy on screen;
  horizontal movement switches to
  `running-left` or `running-right`, downward movement switches to `running`,
  and release returns the buddy to idle.
- After about 20 seconds without user browsing, Match Watch returns to the idle
  clock/time page if the active match is still far away.

### Quick Start

#### Prerequisites

- ESP-IDF is installed and exported
- `ESP-IDF v5.5.4` is recommended

```bash
. <your-esp-idf-path>/export.sh
```

#### Configuration

To make `esp-board-manager` easier to use, first install the helper package with
`pip install esp-bmgr-assist`. You only need to do this once in a given ESP-IDF
environment.

1. Generate board support files:

```bash
cd application/third_party/buddy_pet
idf.py bmgr -c boards -b esp_Ditto
```

`idf.py bmgr -c boards -b <board_name>` (or `gen-bmgr-config`) generates the
configuration for the specified board. Buddy Pet boards are kept under
`application/third_party/buddy_pet/boards`.

If board manager bootstrap fails with
`Failed to resolve esp_board_manager dependency via project dependency graph`,
the underlying cause is usually a stale or corrupt component-manager cache—not a
missing manifest entry. Reset and retry:

```bash
cd application/third_party/buddy_pet
rm -rf managed_components dependencies.lock
idf.py bmgr -c boards -b esp_Ditto
```

If it still fails, print the real resolver error:

```bash
ESP_BMGR_DEBUG=1 idf.py bmgr -c boards -b esp_Ditto
```

Ensure `esp-bmgr-assist` is installed once per ESP-IDF environment:
`pip install esp-bmgr-assist`.

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

## CN

Buddy Pet 是一个基于 ESP-Claw 的产品例程，位于
`application/third_party/buddy_pet/`。它面向 ESP-Ditto 这类带屏设备，
演示如何把 ESP-Claw agent 运行时、微信/IM 控制、本地 Web 配置、
可下载 buddy 包，以及 Match Watch 足球比赛 UI 集成到一个固件应用中。

### 功能集成

- 运行 ESP-Claw event router、agent core、Lua skills、memory 和 capabilities。
- 支持微信/IM 绑定、精确命令路由和自然语言 agent 请求。
- 打开 Match Watch，用于赛程、关注球队、比赛提醒、实时数据或模拟数据展示。
- 下载、安装、选择和驱动 buddy 包动画。
- 提供本地 Web 配置，用于 Wi-Fi、微信绑定、LLM、搜索 key 和时区设置。
- 使用板载 LCD 和触摸控制器完成二维码配置、Match Watch 导航和 buddy 交互。

### IM 聊天

Buddy Pet 同时支持精确 IM 命令和自然语言 prompt。

常用 prompt：

- `/new`：开启新的持久聊天 session。
- `打开比赛`：直接打开 Match Watch UI。
- `打开模拟赛事`：启动模拟 Match Watch provider，并开启比赛提醒。
- `关闭模拟赛事`：停止模拟 provider，并同步回当前 buddy 对应的比赛数据。
- `开启比赛提醒` / `关闭比赛提醒`：开启或关闭 Match Watch 提醒。
- `刷新比赛`：在 Match Watch 已运行时刷新比赛数据。
- `根据当前伙伴打开比赛看板`：打开 Match Watch，并根据当前 buddy profile 同步比赛信息。
- `关注 Brazil World Cup`：通过 Match Watch provider 关注指定球队。
- `下载这个 buddy <url>` / `下载 messi`：下载并选择 buddy 包。
- `切换到 son-heung-min`：选择已安装的 buddy。
- `让兄弟跳一下`：触发 buddy 动作。

完整 IM prompt、Lua 入口和 router event 说明见：

```text
application/third_party/buddy_pet/fatfs_image/skills/LUA_SKILL_COMMANDS.md
```

### 本地设置与触摸交互

本地设置：

- 运行本地 HTTP 配置服务。
- STA Wi-Fi 未连接时，设备屏幕显示标准 Wi-Fi 二维码，对应设备配网 AP，
  例如 `esp-claw-xxxx`。
- 使用手机相机或系统扫码功能扫描该 Wi-Fi 二维码；手机系统会识别为 Wi-Fi
  网络并提示加入设备 AP。加入该 AP 后，再通过 captive portal 或本地配置
  页面提交目标 STA Wi-Fi 凭据。
- 建议在同一个本地配置页面填写 LLM provider 和 API key，这样 IM 自然语言
  请求才能使用 agent 运行时。
- IM 绑定完成后，也可以在聊天里查看或补充 LLM 配置：发送 `/llm status`，
  或使用类似 `/llm setup deepseek sk-xxx deepseek-v4-pro` 的命令。
- Wi-Fi 提交并连接成功后，如果微信凭据尚未就绪，屏幕会切换为微信绑定二维码。
- 微信绑定完成后，凭据会通过 app config 子系统保存，并默认打开 Match Watch。
- 本地配置页面还提供 Wi-Fi、微信、LLM provider、IM 凭据、搜索 key、时区、
  文件和系统设置。
- STA Wi-Fi 不可用时，设备 AP 和 captive setup 页面仍可用于配网。

Match Watch：

- 如果用户还没有手动关注球队，Match Watch 会跟随当前 buddy 的 profile。
  例如当前 buddy 是韩国队，就会打开韩国队赛程。
- 如果你在聊天里关注了其他球队，例如 `关注 Brazil World Cup`，这个球队会
  作为你的选择保存下来，重启后仍会优先显示巴西队；重新加载当前 buddy
  不会把它改回韩国队，除非你明确切换 buddy 或重新关注其他球队。
- Match Watch 时间页点击：进入球队页。
- Match Watch 球队页/详情页点击：在球队视图和详情视图之间切换。
- Match Watch 左滑：下一场比赛。
- Match Watch 右滑：上一场比赛。
- Match Watch 上滑：下一个有比赛的阶段。
- Match Watch 下滑：上一个有比赛的阶段。
- Buddy 拖动：在 buddy 可接收触摸的页面拖动 buddy；横向移动切换到
  `running-left` 或 `running-right`，向下移动切换到 `running`，松手后回到 idle。
- 用户停止浏览约 20 秒后，如果当前比赛仍未临近，Match Watch 会回到 idle
  clock/time 页面。

### 快速开始

#### 前置条件

- 已安装并导出 ESP-IDF
- 推荐使用 `ESP-IDF v5.5.4`

```bash
. <your-esp-idf-path>/export.sh
```

#### 配置

为了更方便使用 `esp-board-manager`，先安装辅助包：
`pip install esp-bmgr-assist`。同一个 ESP-IDF 环境只需要安装一次。

1. 生成板级支持文件：

```bash
cd application/third_party/buddy_pet
idf.py bmgr -c boards -b esp_Ditto
```

`idf.py bmgr -c boards -b <board_name>`（或 `gen-bmgr-config`）会为指定
board 生成配置。Buddy Pet 的 board 定义位于
`application/third_party/buddy_pet/boards`。

如果 board manager bootstrap 报错：
`Failed to resolve esp_board_manager dependency via project dependency graph`，
通常是 component-manager 缓存陈旧或损坏，不是 manifest 缺失。清理后重试：

```bash
cd application/third_party/buddy_pet
rm -rf managed_components dependencies.lock
idf.py bmgr -c boards -b esp_Ditto
```

如果仍然失败，打印真实 resolver 错误：

```bash
ESP_BMGR_DEBUG=1 idf.py bmgr -c boards -b esp_Ditto
```

确认每个 ESP-IDF 环境已安装一次 `esp-bmgr-assist`：
`pip install esp-bmgr-assist`。

2. 配置 Wi-Fi、LLM、IM、搜索引擎和相关参数：

关键 demo 配置包括：

- Wi-Fi SSID / Password
- LLM API Key / Provider / Model
- QQ App ID / App Secret
- Telegram Bot Token
- Brave / Tavily Search Key
- Timezone

说明：

- IM bot token 可从 Telegram [@BotFather](https://t.me/BotFather) 或
  [QQ Bot](https://q.qq.com/qqbot/openclaw/login.html) 获取。
- LLM API key 可从 [Anthropic Console](https://console.anthropic.com)、
  [OpenAI Platform](https://platform.openai.com) 或
  [Alibaba Cloud Bailian](https://bailian.console.aliyun.com/#/api-key) 获取。

可通过 `menuconfig` 调整编译期默认值：

```bash
idf.py menuconfig
```

3. 构建并烧录：

```bash
idf.py build
idf.py flash monitor
```
