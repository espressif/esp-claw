# Lua Skill 命令与 IM Prompt

这份文档记录 Buddy Pet 当前可用的 Lua skill 脚本入口、IM 聊天触发方式，以及 agent 在自然语言场景下应该怎么选择能力。当前产品前台固定为 Match Watch。

## IM 聊天入口

IM 文本消息进入 event router 后，处理顺序大致是：

1. 所有 IM 文本先发送一条处理中回复：

   ```text
   🦞 ESP-Claw is snapping on it...
   ```

2. 如果命中精确短命令，router rule 会直接调用脚本或 capability，并消费消息。
3. 如果没有命中精确短命令，消息会进入 agent。agent 根据 skill 说明选择 Lua 脚本或原生 capability。

精确短命令必须按文案发送。自然语言 prompt 不要求完全一致，但要把“对象、动作、参数”说清楚。

## IM 精确短命令

这些文本已经在 `router_rules/router_rules.json` 中直接配置，适合用户测试和常用操作。

| IM 输入 | 路由动作 | 说明 |
| --- | --- | --- |
| `/new` | `roll_chat_session` | 开启新的持久聊天 session。 |
| `打开比赛` | `match_watch_open` | 打开 Match Watch UI。 |
| `打开模拟赛事` | `match_watch_provider_start` → `match_watch_sim/scripts/watch.lua` + `match_watch_set_reminders(true)` | 启动模拟 provider 并开启比赛提醒。 |
| `关闭模拟赛事` | `match_watch_provider_stop` + `sync_from_pet.lua` | 停止模拟 provider，并按当前 buddy 切回世界杯真实赛程。 |
| `开启比赛提醒` | `match_watch_set_reminders(true)` | 开启 Match Watch 进球/开赛/完赛提醒。 |
| `关闭比赛提醒` | `match_watch_set_reminders(false)` | 关闭 Match Watch 提醒。 |
| `刷新比赛` | `sync_from_pet.lua`（`only_if_running: true`） | Match Watch 已打开时，按当前 buddy 重新启动 provider 拉取数据。 |

## IM 自然语言 Prompt

自然语言 prompt 会进入 agent。推荐写法是：

```text
<动作> + <对象> + <必要参数>
```

好的 prompt 示例：

- `根据当前伙伴打开比赛看板`
- `关注韩国世界杯比赛`
- `关注 Brazil World Cup`
- `开启比赛提醒`
- `刷新比赛`
- `下载这个 buddy：https://codex-pets.net/#/pets/kapi-striker`
- `下载 messi 这个 buddy`
- `切换到 son-heung-min`
- `让兄弟跳一下`

不推荐的 prompt：

- `弄一下`：意图不明确。
- `更新`：不知道更新比赛数据还是 buddy。

### Match Watch Prompt

| 想做的事 | 推荐 IM prompt | agent 应调用 |
| --- | --- | --- |
| 直接打开 UI | `打开比赛` | `match_watch_open` |
| 按当前 buddy 同步比赛 | `根据当前伙伴打开比赛看板`、`同步当前伙伴的赛程` | `match_watch/scripts/sync_from_pet.lua {"open":true}` |
| 关注世界杯球队 | `关注巴西世界杯`、`看阿根廷比赛`、`follow France World Cup` | `match_watch_provider_start` 启动 `match_watch_worldcup/scripts/watch.lua` |
| 开启提醒 | `开启比赛提醒` | `match_watch_set_reminders {"enabled":true}` |
| 关闭提醒 | `关闭比赛提醒` | `match_watch_set_reminders {"enabled":false}` |
| 刷新数据 | `刷新比赛` | `match_watch/scripts/sync_from_pet.lua`（`open:false`, `only_if_running:true`） |
| 模拟赛事 | `打开模拟赛事` | router 精确命令：`match_watch_provider_start` → `match_watch_sim/scripts/watch.lua` |
| provider 模拟赛事 | `启动模拟 provider`、`用模拟比赛数据` | `match_watch_provider_start` 启动 `match_watch_sim/scripts/watch.lua` |

`match_watch/scripts/sync_from_pet.lua` 默认 fast open：先打开 Match Watch UI，再后台启动 World Cup provider 拉真实数据。传 `fast_open=false` 时，才让 provider 拉到数据后再 open。

普通 `sync_from_pet.lua` 同步队伍优先级：

1. Match Watch 当前 `favorite_team`
2. 当前选中 buddy 的 `profile/country`
3. 本地第一个带 `profile/country` 的 buddy
4. `fallback_team`，常用 `Argentina`

buddy 选择事件会传 `prefer_pet_profile=true`，此时优先级调整为：当前选中 buddy 的 `profile/country` -> Match Watch 当前 `favorite_team` -> `fallback_team`。

### Buddy Manager Prompt

| 想做的事 | 推荐 IM prompt | agent 应调用 |
| --- | --- | --- |
| 列出 buddy | `列一下伙伴`、`有哪些 buddy` | `pet_manager/scripts/list_pets.lua` |
| 下载 buddy URL | `下载这个 buddy https://codex-pets.net/#/pets/kapi-striker` | `pet_manager/scripts/download_pet.lua {"url":"..."}` |
| 按 id 下载 buddy | `下载 messi`、`下载 son-heung-min` | `pet_manager/scripts/download_pet.lua {"id":"..."}` |
| 切换 buddy | `切换到 son-heung-min`、`选择 kapi-striker` | `pet_manager/scripts/select_pet.lua {"id":"..."}` |
| 恢复默认 buddy | `恢复默认伙伴`、`清空当前选择的 buddy` | `pet_manager/scripts/clear_pet.lua` |
| buddy 动作 | `让兄弟跳一下`、`buddy 挥手`、`让伙伴向右跑` | `pet_manager/scripts/action_pet.lua` 或 `pet_buddy_action` |

下载 buddy 必须一跳调用 `download_pet.lua`。不要先激活 `skills_lab_search`，不要手动调用 `http_request`、`list_dir`、`read_file`，也不要用 `lua_run_script_async` 轮询下载任务。

选择 buddy 会发布 `pet_buddy pet selected` 事件。当前 router 会在 Match Watch 已运行时自动调用 `match_watch/scripts/sync_from_pet.lua {"open":false,"only_if_running":true,"selected_only":true,"prefer_pet_profile":true}`，用新 buddy 的 `profile/country` 替换 Match Watch provider；如果 Match Watch 没运行，则只切 buddy，不主动打开比赛页。

## 屏幕手动操作

这些是设备屏幕上的现有前台交互，用于补充 IM 命令。IM prompt 里不要把这些描述成必须调用的脚本；它们由当前 UI 场景自己处理。

### Match Watch UI

| 操作 | 生效区域 | 行为 |
| --- | --- | --- |
| 左滑 | 主屏 | 切到下一场比赛。 |
| 右滑 | 主屏 | 切到上一场比赛。 |
| 上滑 | 主屏 | 切到下一个有比赛的阶段。 |
| 下滑 | 主屏 | 切到上一个有比赛的阶段。 |
| 拖动 buddy | buddy 区域 | 拖拽 buddy 跟随手指移动；横向拖动切 `running-left` 或 `running-right`，向下拖动切 `running`，松手回 idle。 |

Match Watch 当前固定显示 team 页。空白点击、favorite 点击和长按切 detail 页已禁用。

### 独立 Buddy Runtime UI

| 操作 | 生效区域 | 行为 |
| --- | --- | --- |
| 拖动 buddy | buddy 区域 | buddy 跟随手指移动；横向拖动切跑动方向，向下拖动切 `running`，松手回 idle。 |

## 公开脚本入口

这些入口可以暴露给普通用户或 agent。它们负责隐藏 provider 接管、buddy 同步等内部细节。

### Buddy Manager

列出已安装的 buddy：

```sh
lua --run --path /fatfs/skills/pet_manager/scripts/list_pets.lua --timeout-ms 3000
```

从 `codex-pets.net` 下载并选中 buddy：

```sh
lua --run --path /fatfs/skills/pet_manager/scripts/download_pet.lua --args-json "{\"url\":\"https://codex-pets.net/#/pets/kapi-striker\"}" --timeout-ms 30000
```

按 buddy id 下载并选中 buddy：

```sh
lua --run --path /fatfs/skills/pet_manager/scripts/download_pet.lua --args-json "{\"id\":\"messi\"}" --timeout-ms 30000
```

选择一个 buddy：

```sh
lua --run --path /fatfs/skills/pet_manager/scripts/select_pet.lua --args-json "{\"id\":\"son-heung-min\"}" --timeout-ms 3000
```

清除当前选择，切回内置/fallback buddy：

```sh
lua --run --path /fatfs/skills/pet_manager/scripts/clear_pet.lua --timeout-ms 3000
```

设置当前 buddy 动作：

```sh
lua --run --path /fatfs/skills/pet_manager/scripts/action_pet.lua --args-json "{\"action\":\"jumping\"}" --timeout-ms 3000
```

常用动作名：

- `idle`
- `jumping`
- `running`
- `running-left`
- `running-right`
- `waving`
- `failed`
- `waiting`
- `review`

`action_pet.lua` 会自动归一化别名，例如 `jump`、`run`、`run_left`、`wave`、`lose`、`wait`。

### Match Watch

打开 Match Watch，并从当前选中的 buddy profile/country 或当前 favorite team 同步比赛信息：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/sync_from_pet.lua --args-json "{\"open\":true}" --timeout-ms 3000
```

只使用当前选中 buddy；如果当前选中项没有 profile/country，则回退到 `fallback_team`：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/sync_from_pet.lua --args-json "{\"open\":true,\"selected_only\":true,\"fallback_team\":\"Argentina\"}" --timeout-ms 3000
```

buddy 选择事件同步 provider 时应优先使用新 buddy 的 profile，而不是旧 favorite：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/sync_from_pet.lua --args-json "{\"open\":false,\"only_if_running\":true,\"selected_only\":true,\"prefer_pet_profile\":true}" --timeout-ms 3000
```

禁用 fast open，等待 provider 拉取并推送数据后再打开：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/sync_from_pet.lua --args-json "{\"open\":true,\"fast_open\":false,\"fallback_team\":\"Argentina\"}" --timeout-ms 3000
```

通过 provider session 关注世界杯球队。这里应使用原生 `match_watch_provider_start`，不要直接 `lua --run`，这样才能干净地替换当前 provider job：

```json
{
  "path": "/fatfs/skills/match_watch_worldcup/scripts/watch.lua",
  "args": {
    "team_slug": "brazil",
    "open": true,
    "poll_ms": 60000,
    "idle_poll_ms": 600000
  },
  "name": "match_watch_worldcup"
}
```

通过 `match_watch_provider_start` 启动模拟 provider：

```json
{
  "path": "/fatfs/skills/match_watch_sim/scripts/watch.lua",
  "args": {
    "open": true,
    "team": "Argentina"
  },
  "name": "match_watch_sim"
}
```

更多 provider 启动和 push data 格式见：

```text
components/common/match_watch_runtime/PROVIDER_CONTRACT_CN.md
```

## 路由/内部事件

这些入口不要作为普通用户可见命令暴露。它们主要用于事件、上层场景脚本或 provider 编排。

STA Wi-Fi 连接后打开 Match Watch，并按当前 buddy 尝试同步比赛：

```text
match_watch network ready -> match_watch/scripts/sync_from_pet.lua
```

buddy 选择后同步正在运行的 Match Watch provider：

```text
pet_buddy pet selected -> match_watch/scripts/sync_from_pet.lua
```

世界杯球队表辅助脚本，由 `openliga_worldcup.lua` 使用：

```text
/fatfs/skills/match_watch_worldcup/scripts/teams.lua
```

## 调试/管理命令

这些命令保留给本地测试使用，但不要在普通 skill 路由里优先选择，因为它们会绕过上层 buddy、场景和 provider 策略。

直接打开 Match Watch UI：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/open.lua --timeout-ms 3000
```

直接打开 Match Watch 并设置 favorite：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/open.lua --args-json "{\"team\":\"Argentina\",\"competition\":\"FIFA World Cup\"}" --timeout-ms 3000
```

通过 buddy 同步入口打开 Match Watch：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/open.lua --args-json "{\"sync_from_pet\":true}" --timeout-ms 3000
```

直接关闭 Match Watch UI：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/close.lua --timeout-ms 3000
```

直接设置 Match Watch favorite：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/set_favorite.lua --args-json "{\"team\":\"Argentina\",\"competition\":\"FIFA World Cup\"}" --timeout-ms 3000
```

直接清除 Match Watch favorite：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/set_favorite.lua --args-json "{}" --timeout-ms 3000
```

直接设置 Match Watch 提醒：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/set_reminders.lua --args-json "{\"enabled\":true}" --timeout-ms 3000
```

直接推送标准化比赛数据：

```sh
lua --run --path /fatfs/skills/match_watch/scripts/push_data.lua --args-json "{\"json\":\"{\\\"schema_version\\\":1,\\\"matches\\\":[]}\"}" --timeout-ms 3000
```

世界杯一次性刷新/调试：

```sh
lua --run --path /fatfs/skills/match_watch_worldcup/scripts/watch.lua --args-json "{\"team_slug\":\"argentina\",\"open\":true,\"max_cycles\":1}" --timeout-ms 30000
```

停止 provider 是原生 capability，不是 Lua 脚本：

```json
{"wait_ms":3000}
```

只有在明确需要停止 provider 轮询时，才直接调用 `match_watch_provider_stop`。
