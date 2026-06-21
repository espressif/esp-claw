# Match Watch Provider Contract

本文档记录 Match Watch provider 和 native runtime 之间的稳定协议：provider 如何启动、哪些事情由 provider 负责，以及 provider 推送给 UI 的标准比赛数据格式。

## 边界

Match Watch native runtime 只接收已经标准化的数据，并负责 UI、提醒、favorite 同步和 provider job 互斥。赛事选择、数据源请求、队伍别名、字段清洗、轮询、重试和是否打开 UI 都属于 provider skill。

`require("match_watch")` 只是 native API 的 Lua binding，不负责 HTTP、赛制解析或轮询策略。

## Provider Session

启动 provider 时使用 `match_watch_provider_start`：

```json
{
  "path": "/fatfs/skills/match_watch_xxx/scripts/watch.lua",
  "args": {},
  "name": "match_watch_xxx"
}
```

字段约定：

- `path`: provider 的 Lua 脚本路径。
- `name`: provider 名称，用于状态和日志。
- `args`: provider 参数，必须是 object；内部字段完全由 provider 自己定义。

C 侧会把 `args` 原样编码后传给 Lua async job，并使用固定的 provider exclusive/replace 语义，保证同一时间只有一个 Match Watch provider 在运行。

provider start 成功后，外层 skill 不应再额外调用：

- `match_watch_open`
- `match_watch_set_favorite`
- `lua_run_script_async`
- `lua_stop_all_async_jobs`
- `web_search`

如果需要打开 UI，把 `open:true` 放进 provider `args`，由 provider 在完成数据 push 后打开 UI。provider 应原子化完成“拉取/生成数据 -> push 标准数据 -> 必要时 open UI”，避免 UI 先打开但没有对应比赛数据。

## Push Data

入口：

- native capability: `match_watch_push_data`
- Lua module: `match_watch.push_data_json(json)`

顶层格式：

```json
{
  "schema_version": 1,
  "provider": "openligadb-worldcup",
  "competition": "FIFA World Cup",
  "team": "Argentina",
  "matches": []
}
```

顶层字段：

- `schema_version`: 新 provider 必须填 `1`。
- `provider`: 数据源标识，用于日志和状态展示。
- `competition`: 比赛名或 provider 展示名。
- `team`: 当前关注队，可选；填写后 C 侧会同步 favorite team。
- `matches`: 标准比赛数组，新 provider 应使用这个唯一入口。
- `group_matches` / `knockout_matches`: 旧兼容入口，新 provider 不再使用。

比赛字段：

```json
{
  "match_no": 1,
  "stage": "group",
  "round": 1,
  "group": "A",
  "home": "Argentina",
  "away": "Brazil",
  "home_code": "ARG",
  "away_code": "BRA",
  "home_display": "阿根廷",
  "away_display": "巴西",
  "date_label": "06.15",
  "time_label": "08:00",
  "beijing_label": "06.15 08:00",
  "venue": "MetLife Stadium",
  "score": "0-0",
  "kickoff_ts": 1781481600,
  "live_minute": 12,
  "state": "live",
  "knockout": false
}
```

必填字段：

- `home`
- `away`

推荐字段：

- `match_no`
- `stage`
- `state`
- `score`
- `kickoff_ts`
- `date_label`
- `time_label`

可选字段：

- `round`
- `group`
- `home_code`
- `away_code`
- `home_display`
- `away_display`
- `beijing_label`
- `venue`
- `live_minute`
- `knockout`

## 枚举

`stage` 推荐值：

- `group`
- `round_of_32`
- `round_of_16`
- `quarter_final`
- `semi_final`
- `third_place`
- `final`

`state` 推荐值：

- `upcoming`
- `live`
- `goal`
- `half_time`
- `full_time`
- `penalty_win`
- `lost`

兼容说明：

- C 侧仍兼容 `finished`、`ended`、`complete` 等旧 state alias，但新 provider 应统一输出 `full_time`。
- 没有可靠比赛分钟时，`live_minute` 填 `0` 或不填。
- `kickoff_ts` 使用 Unix epoch seconds，provider 负责时区转换。
- `score` 推荐使用 `"home-away"`，例如 `"2-1"`；未开赛可用 `"-"` 或 `"0-0"`。
