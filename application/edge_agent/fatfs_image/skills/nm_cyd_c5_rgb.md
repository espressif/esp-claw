# NM-CYD-C5 RGB LED 控制

控制 NM-CYD-C5 板载单颗 WS2812 RGB 灯（GPIO27）。**绝对不要自己写 led_strip / gpio Lua 脚本**，统一调用本脚本即可。

## When to use
当用户要求“开灯/关灯/亮红灯/闪烁/呼吸/彩虹/RGB/灯/LED/灯带/灯珠”等任何指向板载 RGB 灯的请求时使用。

## How to use
始终用一次工具调用即可：

```
run_lua_script script="nm_cyd_c5_rgb" args=<JSON 对象>
```

无需先 activate 任何技能，无需先 write_script，无需 board_manager 或 gpio。

### 常用参数
- `mode` (可选): `solid` | `off` | `blink` | `rainbow`。可省略：给了颜色就是 `solid`，没给任何参数就是 `rainbow` 短演示。
- `color`: 命名色，`red green blue white yellow cyan magenta orange purple pink off`。
- `r`, `g`, `b`: 0..255 直接给原始 RGB（优先级最高）。
- `hue` (0..359), `sat` (0..255), `val` (0..255): HSV 方式。
- `brightness`: 0..255，用于命名色亮度缩放，默认 64。
- 闪烁参数：`count` 次数，`on_ms`/`off_ms` 持续时间。
- 彩虹参数：`duration_ms` 总时长，`step_ms` 步进，`val` 亮度。

### 示例
- 红灯常亮: `{"color":"red"}`
- 暗一点的绿灯: `{"color":"green","brightness":32}`
- 自定义橙色: `{"r":255,"g":120,"b":0}`
- 关灯: `{"mode":"off"}`
- 蓝灯闪 5 次: `{"mode":"blink","color":"blue","count":5,"on_ms":200,"off_ms":200}`
- 2 秒彩虹: `{"mode":"rainbow","duration_ms":2000}`

## 故障恢复
脚本启动时会强制 GC 释放上次未关闭的 RMT 通道，所以即使前一次失败导致 “no free tx channels”，**直接再次调用本脚本**即可恢复，不要尝试别的方案。
